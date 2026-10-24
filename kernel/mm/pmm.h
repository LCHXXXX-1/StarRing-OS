#ifndef PMM_H
#define PMM_H
#include <stdint.h>
#include <stdbool.h>
#include <limine.h>

/* ============================================================
 *  PMM —— 物理页帧分配器
 *
 *  用一张位图管理 Limine 报告的“可用物理内存”，按 4 KiB 页帧分配。
 *  实现细节：
 *   - 位图是静态数组（BSS，128 KiB），最多管理 4 GiB 物理内存，不依赖堆。
 *   - 初始化时先全标“已用”，再把 USABLE 区段标“空闲”，最后保留前 1 MiB
 *     与 0 号页，避免把低端 BIOS 数据/空指针页发出去。
 *   - alloc/free 走 irq_save/restore 临界区，可在中断上下文安全调用。
 * ============================================================ */

#define PMM_PAGE_SIZE   4096u
#define PMM_MAX_FRAMES  (1u << 20)      /* 4 GiB / 4 KiB */

bool     pmm_init(struct limine_memmap_response *resp);
bool     pmm_ready(void);

uint64_t pmm_alloc_frame(void);         /* 返回页帧物理地址；失败返回 0 */
void     pmm_free_frame(uint64_t phys);

uint64_t pmm_total_frames(void);        /* 受管页帧总数 */
uint64_t pmm_free_frames(void);
uint64_t pmm_used_frames(void);

#endif
