#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "lib/mem.h"
#include "arch/x86_64/cpu.h"

/* ================================================================
 *  内核堆分配器 —— 隐式空闲链表
 *
 *  - 16 字节对齐，块头 32 字节（含 magic/size/is_free/debug）。
 *  - First-Fit + Split + Coalesce。
 *  - 通过 pmm_alloc_frame + vmm_map 动态扩展。
 *  - 所有链表操作在 irq_save/irq_restore 临界区内完成。
 * ================================================================ */

#define HEAP_MAGIC      0xDEADBEEFu
#define HEAP_ALIGN      16ull
#define HDR_SIZE        32ull          /* sizeof(block_t) */
#define SPLIT_MIN       (HDR_SIZE * 2) /* 剩余 >= 2 个头才分割 */

/* 堆虚拟地址区间：PML4 index 320，不与内核/HHDM/selftest 冲突 */
#define HEAP_VBASE      0xffffa00000000000ull
#define HEAP_MAX        (128ull * 1024 * 1024)   /* 128 MiB 上限 */
#define HEAP_INIT_PG    16u                       /* 初始 64 KiB */
#define HEAP_EXT_PG     4u                        /* 每次扩展 16 KiB */

typedef struct block {
    uint32_t    magic;
    uint32_t    is_free;
    uint32_t    size;       /* 含本头的总大小，HEAP_ALIGN 的倍数 */
    uint32_t    line;
    const char *file;
    uint64_t    pad;
} __attribute__((aligned(HEAP_ALIGN))) block_t;

_Static_assert(sizeof(block_t) == HDR_SIZE, "block header must be 32 bytes");

static uint64_t heap_brk;     /* 下一个可映射的虚拟地址 */
static bool     heap_ready;

/* ---------- 辅助 ---------- */

static inline uint64_t align_up(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

static inline block_t *block_next(block_t *b) {
    return (block_t *)((uint64_t)b + b->size);
}

static inline bool block_valid(block_t *b) {
    return b->magic == HEAP_MAGIC;
}

static inline bool in_heap(void *p) {
    uint64_t a = (uint64_t)p;
    return a >= HEAP_VBASE && a < heap_brk;
}

/* ---------- 扩展 ---------- */

static bool heap_extend(uint64_t bytes) {
    uint64_t need = align_up(bytes, PMM_PAGE_SIZE);
    uint64_t top  = heap_brk + need;
    if (top > HEAP_VBASE + HEAP_MAX) return false;

    uint64_t v = heap_brk;
    for (; v < top; v += PMM_PAGE_SIZE) {
        uint64_t phys = pmm_alloc_frame();
        if (!phys) goto rollback;
        if (!vmm_map(v, phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITE)) {
            pmm_free_frame(phys);
            goto rollback;
        }
    }

    /* 新页可能落在刚拆分的 2MB 大页/新建的中间层表下，单条 invlpg
       清不干净遗留的大页 TLB 项，这里整表刷新一次（扩展不频繁）。 */
    vmm_flush_tlb();

    block_t *nb = (block_t *)heap_brk;
    nb->magic   = HEAP_MAGIC;
    nb->is_free = 1;
    nb->size    = need;
    nb->line    = 0;
    nb->file    = 0;
    nb->pad     = 0;

    heap_brk = top;
    return true;

rollback:
    /* 失败就把已经建立的映射全部撤销，保持堆区间完整可重试：
       否则 heap_brk 不动、却留下半截映射，下次扩展会踩到旧页。 */
    for (uint64_t u = heap_brk; u < v; u += PMM_PAGE_SIZE) {
        uint64_t phys = 0;
        if (vmm_unmap(u, &phys) && phys) pmm_free_frame(phys);
    }
    vmm_flush_tlb();
    return false;
}

/* ---------- 初始化 ---------- */

bool heap_init(void) {
    if (heap_ready) return true;
    if (!pmm_ready() || !vmm_ready()) return false;

    heap_brk = HEAP_VBASE;
    if (!heap_extend(HEAP_INIT_PG * PMM_PAGE_SIZE)) return false;

    heap_ready = true;
    return true;
}

/* ---------- 分配 ---------- */

void *_kmalloc(uint64_t size, const char *file, int line) {
    if (!heap_ready || size == 0) return 0;
    uint64_t need = align_up(HDR_SIZE + size, HEAP_ALIGN);
    if (need > HEAP_MAX) return 0;

    uint64_t flags = irq_save();

    block_t *cur = (block_t *)HEAP_VBASE;
    for (;;) {
        if (!block_valid(cur)) { irq_restore(flags); return 0; }

        if (cur->is_free && cur->size >= need) {
            /* Split */
            if (cur->size >= need + SPLIT_MIN) {
                block_t *sp = (block_t *)((uint64_t)cur + need);
                sp->magic   = HEAP_MAGIC;
                sp->is_free = 1;
                sp->size    = cur->size - need;
                sp->line    = 0;
                sp->file    = 0;
                sp->pad     = 0;
                cur->size   = need;
            }
            cur->is_free = 0;
            cur->line    = (uint32_t)line;
            cur->file    = file;
            irq_restore(flags);
            return (void *)((uint64_t)cur + HDR_SIZE);
        }

        /* 前进到下一块 */
        block_t *nxt = block_next(cur);
        if ((uint64_t)nxt <= (uint64_t)cur) {      /* size==0：链表损坏 */
            irq_restore(flags);
            return 0;
        }
        if ((uint64_t)nxt >= heap_brk) {
            /* 已遍历全部空闲链表，尝试扩展 */
            uint64_t grow = need > HEAP_EXT_PG * PMM_PAGE_SIZE
                          ? need : HEAP_EXT_PG * PMM_PAGE_SIZE;
            if (!heap_extend(grow)) { irq_restore(flags); return 0; }
            /* 新块在旧 brk 处，继续循环会命中 */
        }
        cur = nxt;
    }
}

/* ---------- 释放 ---------- */

void kfree(void *ptr) {
    if (!ptr || !heap_ready) return;
    if (!in_heap(ptr)) return;

    block_t *b = (block_t *)((uint64_t)ptr - HDR_SIZE);

    uint64_t flags = irq_save();

    if (!block_valid(b))   { irq_restore(flags); return; }  /* 越界/损坏 */
    if (b->is_free)        { irq_restore(flags); return; }  /* 重复释放 */

    b->is_free = 1;

    /* Coalesce with next */
    block_t *nxt = block_next(b);
    while ((uint64_t)nxt < heap_brk && block_valid(nxt) && nxt->is_free) {
        b->size += nxt->size;
        nxt = block_next(b);
    }

    /* Coalesce with previous (implicit list: scan from head) */
    block_t *cur = (block_t *)HEAP_VBASE;
    while (cur != b) {
        block_t *cn = block_next(cur);
        if ((uint64_t)cn >= heap_brk) break;
        if (cn == b && cur->is_free) {
            cur->size += b->size;
            break;
        }
        cur = cn;
    }

    irq_restore(flags);
}

/* ---------- 查询 ---------- */

/* 已分配块的真实容量。realloc 垫片必须用它限制拷贝长度：
   旧实现按“新长度”从旧指针 memcpy，会读到块外（堆尾就是未映射页，
   直接 #PF read err=0）。 */
uint64_t kheap_usable_size(void *ptr) {
    if (!ptr || !heap_ready) return 0;
    if (!in_heap(ptr)) return 0;

    block_t *b = (block_t *)((uint64_t)ptr - HDR_SIZE);
    if (!block_valid(b) || b->is_free) return 0;
    if (b->size <= HDR_SIZE) return 0;
    return (uint64_t)b->size - HDR_SIZE;
}

/* ---------- 统计 ---------- */

uint64_t heap_total(void) {
    return heap_brk - HEAP_VBASE;
}

uint64_t heap_used(void) {
    if (!heap_ready) return 0;
    uint64_t used = 0;
    uint64_t flags = irq_save();
    block_t *cur = (block_t *)HEAP_VBASE;
    while ((uint64_t)cur < heap_brk && block_valid(cur)) {
        if (!cur->is_free) used += cur->size;
        cur = block_next(cur);
    }
    irq_restore(flags);
    return used;
}

uint64_t heap_free(void) {
    return heap_total() - heap_used();
}