#include "idt.h"
#include "drivers/serial.h"
#include "arch/x86_64/cpu.h"

struct idt_entry {
    uint16_t base_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  flags;
    uint16_t base_mid;
    uint32_t base_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   ip;
static isr_handler_t    handlers[256];

/* isr.asm 导出的存根表 */
extern void (*isr_stub_table[])(void);
/* 内部：中断分发器（asm 调用） */
void interrupt_dispatch(struct regs *r);

static void set_gate(int n, uint64_t base, uint16_t sel, uint8_t flags) {
    idt[n].base_low  = base & 0xFFFF;
    idt[n].base_mid  = (base >> 16) & 0xFFFF;
    idt[n].base_high = (base >> 32) & 0xFFFFFFFF;
    idt[n].selector  = sel;
    idt[n].ist       = 0;
    idt[n].flags     = flags;
    idt[n].reserved  = 0;
}

void idt_set_handler(int n, isr_handler_t h) {
    handlers[n] = h;
}

void idt_init(void) {
    /* 使用当前 CS 作为门描述符的段选择子（Limine 下 64 位代码段在 0x28，
       写死 0x08 会导致中断派发时 #GP → 三重故障） */
    uint16_t cs;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));

    for (int i = 0; i < 48; i++)
        set_gate(i, (uint64_t)isr_stub_table[i], cs, 0x8E);

    ip.limit = sizeof(idt) - 1;
    ip.base  = (uint64_t)&idt;
    __asm__ volatile ("lidt %0" : : "m"(ip));
}

/* 由 asm 存根调用的 C 分发器 */
void interrupt_dispatch(struct regs *r) {
    uint64_t n = r->int_no;

    if (handlers[n]) {
        handlers[n](r);
    } else if (n < 32) {
        /* 未处理的 CPU 异常 */
        serial_puts("\n[EXCEPTION] int=");
        serial_putdec(n);
        serial_puts(" err=");
        serial_puthex(r->err_code);
        serial_puts(" rip=");
        serial_puthex(r->rip);
        serial_puts("\n");
        for (;;) hlt();
    }

    /* IRQ 32-47，需要发 EOI */
    if (n >= 32 && n < 48) {
        extern void pic_send_eoi(unsigned char);
        pic_send_eoi((unsigned char)(n - 32));
    }
}
