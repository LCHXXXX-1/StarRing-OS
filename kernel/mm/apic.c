#include "apic.h"
#include "mm/vmm.h"
#include "drivers/serial.h"

/* ------------------------------------------------------------
 *  MMIO 虚拟地址：与 ahci(C0000000) / xhci(C0400000) 错开。
 *  两个页都用 4KB + PCD/PWT 映射（MMIO 必须非缓存）。
 * ------------------------------------------------------------ */
#define LAPIC_VA            0xFFFFFFFFC0800000ull
#define IOAPIC_VA           0xFFFFFFFFC0A00000ull
#define LAPIC_PHYS_DEFAULT  0xFEE00000ull
#define IOAPIC_PHYS_DEFAULT 0xFEC00000ull

#define PTE_PWT 0x008ull
#define PTE_PCD 0x010ull

/* LAPIC 寄存器偏移 */
#define LAPIC_ID    0x020u
#define LAPIC_TPR   0x080u
#define LAPIC_EOI   0x0B0u
#define LAPIC_SVR   0x0F0u

/* IOAPIC 寄存器（经 IOREGSEL/IOWIN 间接访问） */
#define IOAPIC_SEL  0x00u
#define IOAPIC_WIN  0x10u
#define IOAPIC_VER  0x01u
#define IOAPIC_REDTBL_LO(n) (0x10u + (n) * 2u)
#define IOAPIC_REDTBL_HI(n) (0x11u + (n) * 2u)

static volatile uint8_t *g_lapic  = 0;
static volatile uint8_t *g_ioapic = 0;
static bool    g_on = false;
static uint8_t g_id = 0;

static inline uint32_t lapic_rd(uint32_t off) {
    return *(volatile uint32_t *)(g_lapic + off);
}
static inline void lapic_wr(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_lapic + off) = v;
}

static inline uint32_t ioapic_rd(uint32_t reg) {
    *(volatile uint32_t *)(g_ioapic + IOAPIC_SEL) = reg;
    return *(volatile uint32_t *)(g_ioapic + IOAPIC_WIN);
}
static inline void ioapic_wr(uint32_t reg, uint32_t v) {
    *(volatile uint32_t *)(g_ioapic + IOAPIC_SEL) = reg;
    *(volatile uint32_t *)(g_ioapic + IOAPIC_WIN) = v;
}

/* 写一条重定向表项：Fixed 投递、物理目的地、高电平有效、边沿触发。
   ISA 的 PIT / PS/2 都是边沿信号，设电平触发会在 EOI 后重复采样。 */
static void ioapic_route(uint32_t gsi, uint32_t vector) {
    uint32_t lo = vector & 0xFFu;                 /* [7:0]   vector          */
    lo |= 0u << 8;                                /* [10:8]  Fixed           */
    lo |= 0u << 11;                               /* [11]    物理目的地      */
    lo |= 0u << 13;                               /* [13]    高电平有效      */
    lo |= 0u << 15;                               /* [15]    边沿触发        */
    uint32_t hi = (uint32_t)g_id << 24;           /* [63:56] 目的地 APIC ID  */

    ioapic_wr(IOAPIC_REDTBL_HI(gsi), hi);
    ioapic_wr(IOAPIC_REDTBL_LO(gsi), lo | 0x00010000u);  /* 先带 mask 写入 */
    ioapic_wr(IOAPIC_REDTBL_LO(gsi), lo);                /* 再解除 mask    */
}

bool apic_init(void) {
    if (g_on) return true;
    if (!vmm_ready()) {
        serial_puts("[SRCore] apic: vmm not ready\n");
        return false;
    }

    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1B));
    uint64_t base = ((uint64_t)hi << 32) | (lo & 0xFFFFF000ull);
    /* base 字段不合理时用默认值（有些固件在 Legacy 下把整寄存器清 0） */
    if (base < 0x00000000FEC00000ull || base > 0x00000000FFFFF000ull)
        base = LAPIC_PHYS_DEFAULT;

    if (!vmm_map(LAPIC_VA, base,
                 VMM_FLAG_PRESENT | VMM_FLAG_WRITE | PTE_PCD | PTE_PWT)) {
        serial_puts("[SRCore] apic: lapic map failed\n");
        return false;
    }
    vmm_flush_tlb();
    g_lapic = (volatile uint8_t *)(uintptr_t)LAPIC_VA;

    /* 1) 硬件使能：APIC_BASE bit11 */
    if (!(lo & (1u << 11))) {
        lo |= (1u << 11);
        __asm__ volatile ("wrmsr" : : "a"(lo), "d"(hi), "c"(0x1B));
    }

    /* 2) 软件使能：SVR bit8=1。
       spurious vector 必须落在 IDT 已安装的范围内（idt_init 只装 0..47），
       用 0xFF 会越过 IDT 直接 #GP，这里取 0x2F（47，IRQ15 位置，无人使用）。 */
    lapic_wr(LAPIC_SVR, 0x12Fu);
    /* 3) TPR 清零：否则固件留下的高门限会压住所有中断 */
    lapic_wr(LAPIC_TPR, 0x00u);

    g_id = (uint8_t)(lapic_rd(LAPIC_ID) >> 24);
    g_on = (lapic_rd(LAPIC_SVR) & 0x100u) != 0;

    serial_puts("[SRCore] apic: id=");
    serial_putdec((uint64_t)g_id);
    serial_puts(" svr=");
    serial_puthex((uint64_t)lapic_rd(LAPIC_SVR));
    serial_puts(g_on ? " [enabled]\n" : " [ENABLE FAILED]\n");
    return g_on;
}

bool ioapic_init(void) {
    if (!g_on) return false;
    if (!vmm_map(IOAPIC_VA, IOAPIC_PHYS_DEFAULT,
                 VMM_FLAG_PRESENT | VMM_FLAG_WRITE | PTE_PCD | PTE_PWT)) {
        serial_puts("[SRCore] ioapic: map failed\n");
        return false;
    }
    vmm_flush_tlb();
    g_ioapic = (volatile uint8_t *)(uintptr_t)IOAPIC_VA;

    uint32_t ver = ioapic_rd(IOAPIC_VER);
    if ((ver & 0xFFu) < 0x11u || ver == 0xFFFFFFFFu) {
        serial_puts("[SRCore] ioapic: not present\n");
        return false;
    }
    uint32_t maxent = (ver >> 16) & 0xFFu;
    if (maxent > 23u) maxent = 23u;

    /* 先把固件留下的所有重定向项屏蔽，避免 sti 后往随机向量投递 */
    for (uint32_t i = 0; i <= maxent; i++) {
        ioapic_wr(IOAPIC_REDTBL_HI(i), 0x00000000u);
        ioapic_wr(IOAPIC_REDTBL_LO(i), 0x00010000u);   /* mask */
    }

    /* ISA IRQ -> GSI：MP/ACPI 标准映射是 IRQ0→GSI2、IRQ1→GSI1、IRQ12→GSI12
       （IRQ0 不在 GSI0：GSI0 是 8259 级联那条线）。
       向量 = IRQ + 0x20，与 IDT 的 32..47 号门一致。 */
    ioapic_route(2,  32);   /* PIT   → 0x20 */
    ioapic_route(1,  33);   /* 键盘  → 0x21 */
    ioapic_route(12, 44);   /* 鼠标  → 0x2C */

    serial_puts("[SRCore] ioapic: ver=");
    serial_puthex((uint64_t)ver);
    serial_puts(" entries=");
    serial_putdec((uint64_t)(maxent + 1));
    serial_puts(" routed irq0/1/12\n");
    return true;
}

void apic_disable(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1B));
    if (lo & (1u << 11)) {
        lo &= ~(1u << 11);
        __asm__ volatile ("wrmsr" : : "a"(lo), "d"(hi), "c"(0x1B));
    }
    g_on = false;
}

bool apic_enabled(void) { return g_on; }

uint8_t apic_id(void) { return g_id; }

void apic_send_eoi(void) {
    if (g_on && g_lapic) lapic_wr(LAPIC_EOI, 0);
}
