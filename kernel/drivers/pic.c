#include "pic.h"
#include "arch/x86_64/cpu.h"
#include "mm/apic.h"          /* apic_enabled / apic_send_eoi */

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

/* IOAPIC 接管后 8259 被整体屏蔽；此时驱动里的 pic_clear_mask() 必须失效，
   否则 timer_init/keyboard_init 会把 IRQ0/IRQ1 重新打开，造成同一条中断
   既走 IOAPIC 又走 LINT0 ExtINT 双投递（时钟翻倍、按键重复）。 */
static bool g_pic_off = false;

void pic_remap(void) {
    /* 只做 8259 重定位（ICW1~ICW4），把向量挪到 0x20/0x28。
       local APIC 的使能/关闭都由 mm/apic.c 负责，这里绝不再写 MSR ——
       关 APIC 会切断老平台唯一可用的中断路径。 */
    uint8_t m1 = inb(PIC1_DATA), m2 = inb(PIC2_DATA);
    outb(PIC1_CMD, 0x11); outb(PIC2_CMD, 0x11);
    outb(PIC1_DATA, 0x20); outb(PIC2_DATA, 0x28);
    outb(PIC1_DATA, 0x04); outb(PIC2_DATA, 0x02);
    outb(PIC1_DATA, 0x01); outb(PIC2_DATA, 0x01);
    outb(PIC1_DATA, m1); outb(PIC2_DATA, m2);
}

/* IOAPIC 模式：彻底屏蔽两片 8259，并让后续 clear_mask 变成空操作 */
void pic_disable(void) {
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
    g_pic_off = true;
}

void pic_send_eoi(unsigned char irq) {
    /* APIC 模式下必须写 LAPIC EOI：只写 8259 不会清掉 LAPIC 的 ISR 位，
       下一次同级中断会被永久压住（表现为“只来一次中断就死”）。 */
    if (apic_enabled()) {
        apic_send_eoi();
        return;
    }
    if (irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

void pic_clear_mask(unsigned char irq) {
    if (g_pic_off) return;
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (irq < 8) ? irq : (irq - 8);
    outb(port, inb(port) & ~(1 << bit));
}

void pic_set_mask(unsigned char irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (irq < 8) ? irq : (irq - 8);
    outb(port, inb(port) | (1 << bit));
}
