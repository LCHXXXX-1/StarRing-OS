#include "power.h"
#include "arch/x86_64/cpu.h"
#include "drivers/serial.h"
#include "fs/fs.h"

/* ACPI 电源控制端口（SLP_TYP=0 -> S5 软关机）：
   0x604  QEMU (PIIX4/Q35)、0xB004 Bochs、0x4004 VirtualBox */
static void acpi_try_port(uint16_t port, uint16_t value) {
    outw(port, value);
}

void power_shutdown(void) {
    serial_puts("[SRCore] shutdown\n");

    __asm__ volatile ("cli");

    /* 同步文件系统，保证掉电前数据落盘 */
    fs_sync();

    acpi_try_port(0x604,  0x2000);   /* QEMU */
    acpi_try_port(0xB004, 0x2000);   /* Bochs / 旧版 QEMU */
    acpi_try_port(0x4004, 0x3400);   /* VirtualBox */

    /* 平台不支持软关机：停在这里，不再返回桌面 */
    serial_puts("[SRCore] ACPI poweroff unsupported, halted\n");
    __asm__ volatile ("cli");
    for (;;) hlt();
}

void power_reboot(void) {
    serial_puts("[SRCore] reboot\n");
    fs_sync();

    __asm__ volatile ("cli");

    /* 1) i8042 复位线（QEMU/多数 PC 有效） */
    for (int i = 0; i < 100000; i++)
        if (!(inb(0x64) & 0x02)) break;
    outb(0x64, 0xFE);

    /* 2) 端口 0xCF9 的系统复位（芯片组 FADT RESET_REG 常用） */
    outb(0xCF9, 0x06);

    /* 3) 兜底：加载空 IDT 触发三重故障，CPU 自动复位 */
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt;
    null_idt.limit = 0;
    null_idt.base  = 0;
    __asm__ volatile ("lidt %0" : : "m"(null_idt));
    __asm__ volatile ("int3");

    for (;;) hlt();
}
