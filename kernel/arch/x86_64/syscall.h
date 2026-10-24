#ifndef SYSCALL_H
#define SYSCALL_H
#include <stdint.h>
#include <stdbool.h>

/* ============================================================
 *  SYSCALL/SYSRET 快速系统调用
 *
 *  用户程序约定（Linux 式寄存器传参，注意 rcx/r11 被硬件占用）：
 *      rax = 系统调用号
 *      rdi, rsi, rdx, r10, r8, r9 = 参数 1..6   （第 4 个参数是 r10，不是 rcx）
 *      返回值放 rax
 *      陷入后 rcx = 返回 RIP，r11 = 进入时的 RFLAGS（由硬件写入）
 *
 *  返回用户态用 iretq（不是 sysretq），原因见 tss.h 的 GDT 布局说明：
 *  我们要“用户代码段=0x1B / 用户数据段=0x23”，而 sysretq 强制
 *  SS=base+8、CS=base+16，二者顺序相反，无法同时满足。
 * ============================================================ */

/* 系统调用号 */
#define SYS_WRITE   1ull
#define SYS_EXIT    60ull

/* 用户内存窗口（代码页 + 栈页），syscall 里用它校验用户指针 */
#define USER_MEM_BASE   0x400000ull
#define USER_MEM_END    0x402000ull   /* 不含 */

/* asm 入口与内核栈切换点 */
extern void syscall_entry(void);
extern void asm_enter_user(uint64_t entry, uint64_t ustack);

/* 启用 SCE 并配置 STAR / LSTAR / SFMASK。需在 tss_init() 之后调用。 */
void syscall_init(void);

/* 给 #DE/#UD/#GP/#PF 等装上“来自 CPL3 就安全退回内核”的兜底处理。 */
void syscall_install_fault_recovery(void);

/* 标记当前是否正处于 Ring3 执行窗口；进入前置 true。 */
void syscall_set_user_active(bool on);
bool syscall_user_faulted(void);

/* 用户态演示程序：建页 → 载入机器码 → 进入 Ring3 → 打印 → 退回内核。
   成功返回 true。 */
bool usermode_run_demo(void);

/* 越权探针：Ring3 试图读内核镜像地址，验证 U 位隔离与异常兜底。
   返回 true 表示“异常被正确拦下并退回内核”（这是期望结果）。 */
bool usermode_run_probe(void);

#endif
