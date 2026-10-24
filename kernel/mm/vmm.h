#ifndef VMM_H
#define VMM_H
#include <stdint.h>
#include <stdbool.h>

/* ============================================================
 *  VMM —— 虚拟内存（分页）管理
 *
 *  在“当前活动”的 PML4（CR3）上做 4 级页表操作。Limine 已经把内核
 *  映射到高半区并开启了分页，这里不重建页表，只在其之上增量地
 *  map / unmap / 查询，中间层页表按需从 PMM 申请。
 *
 *  访问页表内存依赖 Limine 的 HHDM（物理内存直接映射）：
 *      virtual = physical + hhdm_offset
 *  因此 vmm_init() 必须传入 hhdm offset。
 *
 *  说明：中间层页表帧一旦分配就不再回收（unmap 只清最后一级 PTE），
 *  这对一个演示级 VMM 是够用的，也避免了共享页表的引用计数问题。
 * ============================================================ */

#define VMM_FLAG_PRESENT (1ull << 0)
#define VMM_FLAG_WRITE   (1ull << 1)
#define VMM_FLAG_USER    (1ull << 2)
#define VMM_FLAG_NX      (1ull << 63)

/* HHDM 偏移的本源（读取 Limine 响应，随时可用）。整个内核的
   limine_hhdm_request 只在 boot/limine_requests.c 定义一次；其它模块
   请用本函数，不要重复声明请求。 */
uint64_t vmm_hhdm_offset(void);

bool     vmm_init(uint64_t hhdm_offset);
bool     vmm_ready(void);

uint64_t vmm_cr3(void);
uint64_t vmm_hhdm(void);

/* 建立/覆盖 virt -> phys 映射；virt/phys 必须页对齐 */
bool     vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);
/* 解除映射；out_phys 非空时返回被解除的物理地址 */
bool     vmm_unmap(uint64_t virt, uint64_t *out_phys);
/* 查询映射；返回是否 present，可选返回物理地址与低 12 位标志 */
bool     vmm_translate(uint64_t virt, uint64_t *out_phys, uint64_t *out_flags);

/* 整表刷新（重载 CR3）。批量映射/新建中间层页表后使用。 */
void     vmm_flush_tlb(void);

uint64_t vmm_mapped_pages(void);   /* VMM 经手 present 的页数 */
uint64_t vmm_table_frames(void);   /* VMM 分配的中间层页表帧数 */

/* 清空全地址空间的 USER 位（开启 Ring 3 前的内核加固）。 */
void vmm_clear_user(void);

/* 综合自检：分配页帧 → 选空闲虚拟地址映射 → 写入并读回 → 解除映射 → 释放。
   成功返回 true。供 shell 'ptest' 与系统监视器按钮共用。 */
bool vmm_selftest(void);

#endif
