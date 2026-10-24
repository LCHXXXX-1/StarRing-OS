#include "pmm.h"
#include "arch/x86_64/cpu.h"
#include "lib/mem.h"

/* 1 bit / 页帧：1=已用，0=空闲 */
static uint8_t  bitmap[PMM_MAX_FRAMES / 8];
static uint64_t managed_frames = 0;
static uint64_t free_count     = 0;
static uint64_t next_hint      = 0;
static bool     ready          = false;

static inline void bit_set(uint64_t i)   { bitmap[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static inline void bit_clear(uint64_t i) { bitmap[i >> 3] &= (uint8_t)~(1u << (i & 7)); }
static inline bool bit_test(uint64_t i)  { return (bitmap[i >> 3] >> (i & 7)) & 1u; }

bool pmm_init(struct limine_memmap_response *resp) {
    if (!resp || resp->entry_count == 0) return false;

    /* 1) 默认全部“已用” */
    memset(bitmap, 0xFF, sizeof(bitmap));

    /* 找出最高可用地址，决定受管范围 */
    uint64_t highest = 0;
    for (uint64_t k = 0; k < resp->entry_count; k++) {
        struct limine_memmap_entry *e = resp->entries[k];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t end = e->base + e->length;
        if (end > highest) highest = end;
    }
    if (highest == 0) return false;

    managed_frames = highest / PMM_PAGE_SIZE;
    if (managed_frames > PMM_MAX_FRAMES) managed_frames = PMM_MAX_FRAMES;
    if (managed_frames == 0) return false;

    /* 2) 只把 USABLE 区段标为空闲（受管范围内） */
    free_count = 0;
    for (uint64_t k = 0; k < resp->entry_count; k++) {
        struct limine_memmap_entry *e = resp->entries[k];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;

        uint64_t first = (e->base + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
        uint64_t last  = (e->base + e->length) / PMM_PAGE_SIZE;   /* 不含 */
        if (last > managed_frames) last = managed_frames;
        for (uint64_t f = first; f < last; f++) {
            if (!bit_test(f)) continue;
            bit_clear(f);
            free_count++;
        }
    }

    /* 3) 保留低端 1 MiB（含 0 号页） */
    uint64_t low = 0x100000u / PMM_PAGE_SIZE;
    if (low > managed_frames) low = managed_frames;
    for (uint64_t f = 0; f < low; f++) {
        if (bit_test(f)) continue;
        bit_set(f);
        if (free_count) free_count--;
    }

    next_hint = low;
    ready = true;
    return true;
}

bool pmm_ready(void) { return ready; }

uint64_t pmm_alloc_frame(void) {
    if (!ready) return 0;

    uint64_t flags = irq_save();
    uint64_t found = 0;                 /* 0 = 未找到（帧 0 永远保留） */
    for (uint64_t i = 0; i < managed_frames; i++) {
        uint64_t idx = next_hint + i;
        if (idx >= managed_frames) idx -= managed_frames;
        if (bit_test(idx)) continue;
        bit_set(idx);
        free_count--;
        next_hint = idx + 1;
        if (next_hint >= managed_frames) next_hint = 0;
        found = idx;
        break;
    }
    irq_restore(flags);

    return found * PMM_PAGE_SIZE;
}

void pmm_free_frame(uint64_t phys) {
    if (!ready) return;
    uint64_t idx = phys / PMM_PAGE_SIZE;
    if (idx == 0 || idx >= managed_frames) return;

    uint64_t flags = irq_save();
    if (bit_test(idx)) {
        bit_clear(idx);
        free_count++;
    }
    irq_restore(flags);
}

uint64_t pmm_total_frames(void) { return managed_frames; }
uint64_t pmm_free_frames(void)  { return free_count; }
uint64_t pmm_used_frames(void)  { return managed_frames - free_count; }
