#include "vmm.h"
#include "pmm.h"
#include "arch/x86_64/cpu.h"
#include "boot/limine_requests.h"
#include <limine.h>

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ull
#define PTE_PS        (1ull << 7)          /* 2 MiB / 1 GiB 大页 */

/* Limine 对同一个 request ID 出现多次会直接 PANIC
   ("Conflict detected for request ID ...")，所以整个内核只能有一份 HHDM
   请求，统一放在 boot/limine_requests.c（g_hhdm_req）。
   这里只负责把它包装成 vmm_hhdm_offset() 供其它模块使用。 */

uint64_t vmm_hhdm_offset(void) {
    return g_hhdm_req.response ? g_hhdm_req.response->offset : 0;
}

static uint64_t hhdm  = 0;
static uint64_t cr3   = 0;
static bool     ready = false;

static uint64_t mapped_pages = 0;
static uint64_t table_frames = 0;

static inline uint64_t *ptrs(uint64_t phys) {
    return (uint64_t *)(phys + hhdm);
}

/* 页表项里的物理地址必须落在 PMM 管理的 RAM 内且 4K 对齐。
   页表一旦被写坏（例如栈溢出踩到 .bss 里的 cr3/hhdm），继续拿这个
   地址 + hhdm 去读页表会直接 #PF(int=14, err=0)。这里一律当作
   “不可用”，让上层安全失败，而不是崩在内核里。 */
static bool paddr_ok(uint64_t pa) {
    if (!pa || (pa & 0xFFFull)) return false;
    uint64_t limit = pmm_total_frames() * PMM_PAGE_SIZE;
    return pa < limit;
}

/* 从当前层页表 tbl 取 idx 项指向的下一层页表；create 时按需分配 */
static uint64_t *descend(uint64_t *tbl, uint64_t idx, bool create) {
    uint64_t e = tbl[idx];
    if (e & 1) {
        if (e & PTE_PS) return 0;                  /* 大页：不在此处拆分 */
        uint64_t pa = e & PTE_ADDR_MASK;
        if (!paddr_ok(pa)) return 0;               /* 表项已损坏 */
        return ptrs(pa);
    }
    if (!create) return 0;

    uint64_t nf = pmm_alloc_frame();
    if (!nf) return 0;

    uint64_t *nt = ptrs(nf);
    for (int i = 0; i < 512; i++) nt[i] = 0;
    tbl[idx] = (nf & PTE_ADDR_MASK) | VMM_FLAG_PRESENT | VMM_FLAG_WRITE;
    table_frames++;
    return nt;
}

/* level: 1 = PDPT 项（1 GiB 大页）拆成 PD；2 = PD 项（2 MiB 大页）拆成 PT。
   拆分保留原大页的物理基址与低 12 位标志（去掉 PS 位），逐子项铺好，
   这样覆盖大页里的某一个 4 KiB 时不会破坏其余 511 个映射。 */
static uint64_t *split_large(uint64_t *tbl, uint64_t idx, int level) {
    uint64_t e = tbl[idx];
    uint64_t base = e & PTE_ADDR_MASK;
    uint64_t low  = (e & 0xFFFull) & ~PTE_PS;      /* 继承标志，去掉 PS */

    uint64_t nf = pmm_alloc_frame();
    if (!nf) return 0;
    uint64_t *nt = ptrs(nf);
    table_frames++;

    uint64_t step = (level == 2) ? 0x1000ull : 0x200000ull;   /* 4K 子页 / 2M 子页 */
    uint64_t extra = (level == 1) ? PTE_PS : 0;               /* 拆 PD 时子项仍是 2M 大页 */
    for (int i = 0; i < 512; i++)
        nt[i] = (base + (uint64_t)i * step) | low | extra;

    tbl[idx] = (nf & PTE_ADDR_MASK) | VMM_FLAG_PRESENT | VMM_FLAG_WRITE |
               (e & VMM_FLAG_USER);
    return nt;
}

/* 逐级向下并确保存在一张 4 KiB 级页表；pathflags 里若带 USER，则沿途的
   中间层表项也一并置 USER，否则 Ring 3 无法穿越非叶层（U=0 会先于叶层拦下）。 */
static uint64_t *walk_to_pt(uint64_t virt, uint64_t pathflags) {
    uint64_t *pml4 = ptrs(cr3);
    uint64_t i3 = (virt >> 39) & 0x1FF;
    uint64_t i2 = (virt >> 30) & 0x1FF;
    uint64_t i1 = (virt >> 21) & 0x1FF;

    uint64_t e = pml4[i3];
    uint64_t *pdpt;
    if (!(e & VMM_FLAG_PRESENT)) {
        pdpt = descend(pml4, i3, true);
        if (pdpt) pml4[i3] |= pathflags & VMM_FLAG_USER;
    } else if (e & PTE_PS) {
        return 0;                                   /* PML4 直接是大页：本内核不处理 */
    } else {
        uint64_t pa = e & PTE_ADDR_MASK;
        if (!paddr_ok(pa)) return 0;                /* 表项损坏：安全失败 */
        pdpt = ptrs(pa);
        pml4[i3] |= pathflags & VMM_FLAG_USER;
    }
    if (!pdpt) return 0;

    e = pdpt[i2];
    uint64_t *pd;
    if (!(e & VMM_FLAG_PRESENT)) {
        pd = descend(pdpt, i2, true);
        if (pd) pdpt[i2] |= pathflags & VMM_FLAG_USER;
    } else if (e & PTE_PS) {
        pd = split_large(pdpt, i2, 1);             /* 1 GiB → 2 MiB 表 */
        if (pd) pdpt[i2] |= pathflags & VMM_FLAG_USER;
    } else {
        uint64_t pa = e & PTE_ADDR_MASK;
        if (!paddr_ok(pa)) return 0;
        pd = ptrs(pa);
        pdpt[i2] |= pathflags & VMM_FLAG_USER;
    }
    if (!pd) return 0;

    e = pd[i1];
    uint64_t *pt;
    if (!(e & VMM_FLAG_PRESENT)) {
        pt = descend(pd, i1, true);
        if (pt) pd[i1] |= pathflags & VMM_FLAG_USER;
    } else if (e & PTE_PS) {
        pt = split_large(pd, i1, 2);               /* 2 MiB → 4 KiB 表 */
        if (pt) pd[i1] |= pathflags & VMM_FLAG_USER;
    } else {
        uint64_t pa = e & PTE_ADDR_MASK;
        if (!paddr_ok(pa)) return 0;
        pt = ptrs(pa);
        pd[i1] |= pathflags & VMM_FLAG_USER;
    }
    return pt;
}

bool vmm_init(uint64_t hhdm_offset) {
    hhdm = hhdm_offset;

    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    cr3 = v & PTE_ADDR_MASK;
    if (cr3 == 0) return false;

    ready = true;
    return true;
}

bool vmm_ready(void)    { return ready; }
uint64_t vmm_cr3(void)  { return cr3; }
uint64_t vmm_hhdm(void) { return hhdm; }

/* 整表刷新：新建/拆分中间层页表、或一次映射多页后调用最稳。
   （单条 invlpg 只清一个地址，清不掉新大页/新中间层的旧 TLB 项。） */
void vmm_flush_tlb(void) {
    uint64_t c;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(c));
    __asm__ volatile ("mov %0, %%cr3" :: "r"(c) : "memory");
}

bool vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!ready) return false;
    if ((virt | phys) & 0xFFFull) return false;

    uint64_t irqf = irq_save();

    uint64_t *pt = walk_to_pt(virt, flags);

    bool ok = false;
    if (pt) {
        uint64_t i1 = (virt >> 12) & 0x1FF;
        uint64_t e = (phys & PTE_ADDR_MASK) | (flags & 0xFFFull) | VMM_FLAG_PRESENT;
        if (flags & VMM_FLAG_NX) e |= VMM_FLAG_NX;
        if (pt[i1] & VMM_FLAG_PRESENT) {
            /* 覆盖已有映射：present 计数不变 */
        } else {
            mapped_pages++;
        }
        pt[i1] = e;
        ok = true;
    }

    irq_restore(irqf);

    if (ok) {
        if (flags & VMM_FLAG_USER) {
            /* 建用户页时可能刚拆分了大页/新建了中间层表，单条 invlpg
               不足以清掉遗留的大页 TLB 项，直接重载 CR3 最稳。 */
            vmm_flush_tlb();
        } else {
            __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
        }
    }
    return ok;
}

/* 把整个地址空间所有 present 的表项（含中间层与大页）的 USER 位清掉，
   作为开启 Ring 3 前的加固：默认全部是 supervisor-only，之后只有我们显式
   映射的用户页才会重新带上 USER。 */
static void clear_user_table(uint64_t *tbl, int level) {
    for (int i = 0; i < 512; i++) {
        uint64_t e = tbl[i];
        if (!(e & VMM_FLAG_PRESENT)) continue;
        tbl[i] = e & ~VMM_FLAG_USER;
        if (level > 0 && !(e & PTE_PS))
            clear_user_table(ptrs(e & PTE_ADDR_MASK), level - 1);
    }
}

void vmm_clear_user(void) {
    if (!ready) return;
    uint64_t irqf = irq_save();
    clear_user_table(ptrs(cr3), 3);
    irq_restore(irqf);
    /* 清了中间层 USER，需刷新 TLB；最简单是重载 CR3 */
    vmm_flush_tlb();
}

bool vmm_unmap(uint64_t virt, uint64_t *out_phys) {
    if (!ready) return false;
    if (virt & 0xFFFull) return false;

    uint64_t irqf = irq_save();

    uint64_t *pml4 = ptrs(cr3);
    uint64_t *pdpt = descend(pml4, (virt >> 39) & 0x1FF, false);
    uint64_t *pd   = pdpt ? descend(pdpt, (virt >> 30) & 0x1FF, false) : 0;
    uint64_t *pt   = pd   ? descend(pd,   (virt >> 21) & 0x1FF, false) : 0;

    bool ok = false;
    if (pt) {
        uint64_t i1 = (virt >> 12) & 0x1FF;
        uint64_t e = pt[i1];
        if (e & VMM_FLAG_PRESENT) {
            if (out_phys) *out_phys = e & PTE_ADDR_MASK;
            pt[i1] = 0;
            if (mapped_pages) mapped_pages--;
            ok = true;
        }
    }

    irq_restore(irqf);

    if (ok) __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
    return ok;
}

bool vmm_translate(uint64_t virt, uint64_t *out_phys, uint64_t *out_flags) {
    if (!ready) return false;

    uint64_t e = ptrs(cr3)[(virt >> 39) & 0x1FF];
    if (!(e & VMM_FLAG_PRESENT)) return false;

    e = ptrs(e & PTE_ADDR_MASK)[(virt >> 30) & 0x1FF];
    if (!(e & VMM_FLAG_PRESENT)) return false;
    if (e & PTE_PS) {                          /* 1 GiB 大页 */
        if (out_phys)  *out_phys  = (e & PTE_ADDR_MASK & ~0x3FFFFFFFull) |
                                    (virt & 0x3FFFFFFFull);
        if (out_flags) *out_flags = e & 0xFFFull;
        return true;
    }

    e = ptrs(e & PTE_ADDR_MASK)[(virt >> 21) & 0x1FF];
    if (!(e & VMM_FLAG_PRESENT)) return false;
    if (e & PTE_PS) {                          /* 2 MiB 大页 */
        if (out_phys)  *out_phys  = (e & PTE_ADDR_MASK & ~0x1FFFFFull) |
                                    (virt & 0x1FFFFFull);
        if (out_flags) *out_flags = e & 0xFFFull;
        return true;
    }

    uint64_t pte = ptrs(e & PTE_ADDR_MASK)[(virt >> 12) & 0x1FF];
    if (!(pte & VMM_FLAG_PRESENT)) return false;

    if (out_phys)  *out_phys  = (pte & PTE_ADDR_MASK) | (virt & 0xFFFull);
    if (out_flags) *out_flags = pte & 0xFFFull;
    return true;
}

uint64_t vmm_mapped_pages(void) { return mapped_pages; }
uint64_t vmm_table_frames(void) { return table_frames; }

bool vmm_selftest(void) {
    if (!ready || !pmm_ready()) return false;

    uint64_t frame = pmm_alloc_frame();
    if (!frame) return false;

    /* 在很少使用的 PML4 槽位（0xffff9000_00000000 起）挑一个未映射地址 */
    uint64_t base = 0xffff900000000000ull;
    uint64_t va = 0;
    for (int k = 0; k < 64; k++) {
        uint64_t a = base + (uint64_t)k * 4096ull;
        if (!vmm_translate(a, 0, 0)) { va = a; break; }
    }
    if (!va) { pmm_free_frame(frame); return false; }

    if (!vmm_map(va, frame, VMM_FLAG_WRITE)) {
        pmm_free_frame(frame);
        return false;
    }

    const uint64_t magic = 0x5354415252494E47ull;      /* "STARRING" */
    volatile uint64_t *p = (volatile uint64_t *)va;
    *p = magic;
    uint64_t back = *p;

    uint64_t got = 0;
    bool tr = vmm_translate(va, &got, 0);

    vmm_unmap(va, 0);
    pmm_free_frame(frame);

    return back == magic && tr && got == frame;
}
