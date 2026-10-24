#ifndef IDT_H
#define IDT_H
#include <stdint.h>

/* 中断时的寄存器快照，字段顺序必须和 isr.asm 里 push 的顺序严格对应 */
struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*isr_handler_t)(struct regs *);

void idt_init(void);
void idt_set_handler(int n, isr_handler_t h);

#endif
