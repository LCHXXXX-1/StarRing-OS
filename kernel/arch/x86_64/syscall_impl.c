#include "syscall.h"
#include "tss.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/idt.h"
#include "gfx/console.h"
#include "drivers/serial.h"
#include <stddef.h>

/* 字段顺序必须与 syscall.asm 里 push 的逆序严格一致（与 idt.h 的 regs 同构） */
struct syscall_regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
};

/* asm 出口检查：非 0 表示本次中断/异常需要直接退回内核，而不是 iretq 回 Ring3 */
volatile uint8_t g_user_abort = 0;

/* 是否正处于“内核已跳进 Ring3、尚未返回”的窗口内 */
static volatile bool g_user_active  = false;
static volatile bool g_user_faulted = false;

/* ---- 用户指针粗校验：只允许落在用户内存窗口内，防止拿内核地址骗我们读 ---- */
static bool user_ptr_ok(uint64_t addr, uint64_t len) {
    if (addr < USER_MEM_BASE || addr >= USER_MEM_END) return false;
    if (len > USER_MEM_END - addr) return false;
    return true;
}

/* sys_write(fd, buf, len[, ...])：把用户缓冲区里的字节同时打到串口与屏幕控制台。
   fd 目前忽略（都往同一处输出），保留形参以贴近 POSIX 语义。 */
static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)fd; (void)a4; (void)a5; (void)a6;
    if (!user_ptr_ok(buf, len)) {
        serial_puts("[SRCore] sys_write: bad user pointer\n");
        return (uint64_t)-1;
    }
    const char *p = (const char *)buf;
    for (uint64_t i = 0; i < len; i++) {
        console_putc(p[i]);
        serial_putc(p[i]);
    }
    return len;
}

/* ---- 系统调用表 ----
   参数顺序遵循 syscall.h 的约定：rdi, rsi, rdx, r10, r8, r9。
   新增系统调用 = 写一个同签名函数 + 在表里加一行（编号见 syscall.h）。 */
typedef uint64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t,
                                 uint64_t, uint64_t, uint64_t);

static const syscall_fn_t syscall_table[] = {
    [SYS_WRITE] = sys_write,
};

#define SYSCALL_TABLE_LEN (sizeof(syscall_table) / sizeof(syscall_table[0]))

/* 由 syscall_entry 调用（运行在系统调用内核栈上，IF 已被 SFMASK 关掉） */
void syscall_dispatch(struct syscall_regs *r) {
    if (r->rax < SYSCALL_TABLE_LEN && syscall_table[r->rax]) {
        r->rax = syscall_table[r->rax](r->rdi, r->rsi, r->rdx,
                                       r->r10, r->r8, r->r9);
        return;
    }
    serial_puts("[SRCore] unknown syscall: ");
    serial_puthex(r->rax);
    serial_puts("\n");
    r->rax = (uint64_t)-1;
}

/* SYS_EXIT 由汇编入口直接处理，这里只负责打印退出码，便于串口验证 */
void syscall_log_exit(uint64_t code) {
    serial_puts("[SRCore] ring3 program exited, code=");
    serial_putdec(code);
    serial_puts("\n");
}

/* ---------------- Ring3 异常兜底 ----------------
   没有这段时，用户程序一条野指针就会走到 idt.c 的“未处理异常”分支
   然后 for(;;) hlt()，整个系统跟着挂死 —— 这恰恰是 Ring3 隔离最不该
   出现的失败模式。这里把来自 CPL3 的异常变成“安全退回内核”：

     - 若 CS.RPL == 3：记录向量/错误码/CR2，置 g_user_abort；
       isr_common 的出口发现该标志后不做 iretq，而是恢复进入用户态
       前保存的内核栈并 ret，直接回到 asm_enter_user 的调用者；
     - 若 CS.RPL == 0（真内核 bug）：保持原有 halt 行为，不掩盖问题。 */
static const char *exc_name(uint64_t n) {
    switch (n) {
    case 0:  return "divide error";
    case 6:  return "invalid opcode";
    case 11: return "segment not present";
    case 12: return "stack fault";
    case 13: return "general protection";
    case 14: return "page fault";
    case 17: return "alignment check";
    default: return "exception";
    }
}

static void user_exception(struct regs *r) {
    if ((r->cs & 3) != 3 || !g_user_active) {
        /* 内核态异常：如实报告并停机，避免把内核 bug 当成用户问题吞掉 */
        serial_puts("\n[KERNEL EXCEPTION] ");
        serial_puts(exc_name(r->int_no));
        serial_puts(" int=");
        serial_putdec(r->int_no);
        serial_puts(" err=");
        serial_puthex(r->err_code);
        serial_puts(" rip=");
        serial_puthex(r->rip);
        serial_puts("\n");
        for (;;) hlt();
    }

    uint64_t cr2 = 0;
    if (r->int_no == 14) __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

    serial_puts("[SRCore] USER FAULT contained: ");
    serial_puts(exc_name(r->int_no));
    serial_puts(" err=");
    serial_puthex(r->err_code);
    if (r->int_no == 14) {
        serial_puts(" cr2=");
        serial_puthex(cr2);
    }
    serial_puts(" rip=");
    serial_puthex(r->rip);
    serial_puts(" -> aborting Ring3 program\n");

    g_user_faulted = true;
    g_user_active  = false;

    /* 交给 isr_common 的出口处理：它会放弃当前 iretq 帧，恢复进入用户态
       之前的内核栈并 ret 回 asm_enter_user 的调用者。相比在异常处理里
       改写 CS/RIP 去做跨特权级 iretq，这条路不依赖 iretq 的段校验细节，
       也不会把用户栈/内核栈指针弄错。 */
    g_user_abort = 1;
}

void syscall_install_fault_recovery(void) {
    static const int vec[] = { 0, 6, 11, 12, 13, 14, 17 };
    for (size_t i = 0; i < sizeof(vec) / sizeof(vec[0]); i++)
        idt_set_handler(vec[i], user_exception);
}

void syscall_set_user_active(bool on) {
    if (on) g_user_faulted = false;
    g_user_active = on;
}

bool syscall_user_faulted(void) { return g_user_faulted; }

void syscall_init(void) {
    /* 1) 打开 SYSCALL/SYSRET 使能位（EFER.SCE，bit0） */
    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | 1ull);

    /* 2) STAR：[47:32]=SYSCALL 后的内核 CS（SS 自动 = CS+8）
            [63:48]=SYSRET 的用户段基址——本内核用 iretq 返回，仅占位 */
    wrmsr(MSR_STAR, ((uint64_t)SEL_KERNEL_CODE << 32) |
                    ((uint64_t)SEL_USER_DATA    << 48));

    /* 3) LSTAR：64 位 SYSCALL 入口 */
    wrmsr(MSR_LSTAR, (uint64_t)&syscall_entry);

    /* 4) SFMASK：进入内核时清 IF(bit9) 与 TF(bit8)，避免系统调用处理被打断 */
    wrmsr(MSR_SFMASK, 0x300ull);

    serial_puts("[SRCore] syscall msrs ok (lstar enabled)\n");
}
