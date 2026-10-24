#ifndef CPU_H
#define CPU_H
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void io_delay(void) {   /* ATA 选盘后需 ~400ns 延时 */
    for (int i = 0; i < 4; i++) inb(0x80);
}
static inline void hlt(void) { __asm__ volatile ("hlt"); }

/* ---- MSR 读写 ---- */
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (uint64_t)lo | ((uint64_t)hi << 32);
}
static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile ("wrmsr"
        :
        : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

/* ---- 常用 MSR 编号 ---- */
#define MSR_EFER        0xC0000080u   /* 扩展特性寄存器，bit0 = SCE（启用 syscall/sysret） */
#define MSR_STAR        0xC0000081u   /* SYSCALL/SYSRET 段选择子 */
#define MSR_LSTAR       0xC0000082u   /* SYSCALL 入口 RIP */
#define MSR_CSTAR       0xC0000083u   /* 兼容模式入口（本内核不用） */
#define MSR_SFMASK      0xC0000084u   /* SYSCALL 进入时清空的 RFLAGS 位 */

/* 关中断并返回原 RFLAGS，供极短的临界区使用（如快照/清除共享状态） */
static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    __asm__ volatile ("pushq %0; popfq" :: "r"(f) : "memory", "cc");
}
#endif
