#ifndef APIC_H
#define APIC_H
#include <stdint.h>
#include <stdbool.h>

/* ============================================================
 *  Local APIC + IOAPIC 初始化（Phase 2）
 *
 *  老平台（HM55 一代）的 BIOS 常把 ISA 中断接到 IOAPIC，而 8259 的
 *  INTR 不再直连 CPU。此时必须同时具备：
 *    1) local APIC 硬件使能（APIC_BASE bit11）+ 软件使能（SVR bit8）
 *    2) IOAPIC 重定向表把 ISA IRQ 映射到 IDT 向量
 *    3) 中断结束写 LAPIC EOI（不是 8259 EOI）
 *  任何一步缺失都表现为“首帧能画、hlt 永不唤醒、时钟不走”。
 *
 *  回退：ioapic_init() 失败时 main 调 apic_disable()，回到
 *  “8259 直连 CPU”的基线路径（QEMU 老路径），保证不回归。
 * ============================================================ */

bool    apic_init(void);      /* 映射 LAPIC、使能两级开关；失败返回 false */
bool    ioapic_init(void);    /* 映射 IOAPIC，编程 ISA IRQ 重定向表 */
void    apic_disable(void);   /* 回退：清 APIC_BASE bit11，交回 8259 */
bool    apic_enabled(void);
uint8_t apic_id(void);        /* 本地 APIC ID（BSP） */
void    apic_send_eoi(void);  /* 向 LAPIC 写 EOI；未启用时为空操作 */
#endif
