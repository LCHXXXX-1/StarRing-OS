#include "timer.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/cpu.h"
#include "drivers/pic.h"
#include "drivers/serial.h"

#define PIT_INPUT_HZ 1193182u
#define PIT_TICK_HZ  100u

static volatile uint64_t ticks = 0;
static void (*tick_hook)(uint64_t) = 0;

static void timer_handler(struct regs *r) {
    (void)r;
    ticks++;
    if (tick_hook) tick_hook(ticks);
}

void timer_set_hook(void (*fn)(uint64_t)) { tick_hook = fn; }

void timer_init(void) {
    uint32_t divisor = PIT_INPUT_HZ / PIT_TICK_HZ;
    outb(0x43, 0x36);                       /* 通道 0, lo/hi, 方波 */
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));

    idt_set_handler(32, timer_handler);
    pic_clear_mask(0);
}

uint64_t timer_ticks(void)   { return ticks; }
uint64_t timer_seconds(void) { return ticks / PIT_TICK_HZ; }