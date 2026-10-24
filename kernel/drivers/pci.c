#include "pci.h"
#include "arch/x86_64/cpu.h"
#include "drivers/serial.h"

/* 构造 0xCF8 的 CONFIG_ADDRESS：
   [31]    = 使能位
   [23:16] = bus
   [15:11] = slot(设备号)
   [10:8]  = func
   [7:0]   = 寄存器偏移（dword 对齐） */
static uint32_t pci_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return 0x80000000u
         | ((uint32_t)bus  << 16)
         | ((uint32_t)slot << 11)
         | ((uint32_t)func << 8)
         | ((uint32_t)off & 0xFCu);
}

uint32_t pci_read_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, off));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read_config_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_read_config_dword(bus, slot, func, (uint8_t)(off & 0xFCu));
    return (uint16_t)((v >> ((off & 2u) * 8u)) & 0xFFFFu);
}

void pci_write_config_dword(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t off, uint32_t val) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, off));
    outl(PCI_CONFIG_DATA, val);
}

void pci_write_config_word(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t off, uint16_t val) {
    uint8_t  base = (uint8_t)(off & 0xFCu);
    uint32_t v    = pci_read_config_dword(bus, slot, func, base);
    if (off & 2u) v = (v & 0x0000FFFFu) | ((uint32_t)val << 16);
    else          v = (v & 0xFFFF0000u) | (uint32_t)val;
    pci_write_config_dword(bus, slot, func, base, v);
}

/* 探测单个 (bus,slot,func)，若是 AHCI 控制器则打印信息并返回 BAR5，否则 0 */
static uint32_t pci_check_func(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t id = pci_read_config_dword(bus, slot, func, PCI_CFG_VENDOR_ID);
    uint16_t vendor = (uint16_t)(id & 0xFFFFu);
    uint16_t device = (uint16_t)(id >> 16);
    if (vendor == 0xFFFFu) return 0;              /* 无设备 */

    uint32_t cls = pci_read_config_dword(bus, slot, func, PCI_CFG_REVISION);
    uint8_t  prog_if  = (uint8_t)((cls >> 8)  & 0xFFu);
    uint8_t  subclass = (uint8_t)((cls >> 16) & 0xFFu);
    uint8_t  class    = (uint8_t)((cls >> 24) & 0xFFu);

    if (class != PCI_CLASS_STORAGE || subclass != PCI_SUBCLASS_SATA ||
        prog_if != PCI_PROGIF_AHCI)
        return 0;

    /* BAR5 = ABAR（内存映射基址），低 4 位是标志位，须清零 */
    uint32_t bar5 = pci_read_config_dword(bus, slot, func, PCI_CFG_BAR5) & ~0xFu;

    /* 自己把「内存空间译码 + 总线主控」打开，别赌固件帮你做好了的假设：
         MEM_SPACE   = 0 → 读 ABAR 全是 0xFFFFFFFF，控制器像不存在
         BUS_MASTER  = 0 → 命令照样能写进 PxCI，但 HBA 无法取命令列表、
                        无法搬 PRD 数据，命令永不退休，表现就是
                        [ahci] command timeout / write timeout。
       VMware 从光驱引导、内核引导阶段根本没碰过这块 SATA 盘时，固件很常见
       就不会启用 Bus Master —— 这正是“空盘首次写盘就超时”的一个直接原因。 */
    uint16_t cmd  = pci_read_config_word(bus, slot, func, PCI_CFG_COMMAND);
    uint16_t want = (uint16_t)(cmd | PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);
    if (want != cmd) {
        pci_write_config_word(bus, slot, func, PCI_CFG_COMMAND, want);
        cmd = pci_read_config_word(bus, slot, func, PCI_CFG_COMMAND);
    }

    serial_puts("[SRCore] AHCI controller @ bus=");
    serial_putdec(bus);
    serial_puts(" slot=");
    serial_putdec(slot);
    serial_puts(" func=");
    serial_putdec(func);
    serial_puts(" vendor=");
    serial_puthex(vendor);
    serial_puts(" device=");
    serial_puthex(device);
    serial_puts(" ABAR=");
    serial_puthex(bar5);
    serial_puts(" command=");
    serial_puthex(cmd);
    serial_puts((cmd & PCI_CMD_BUS_MASTER) ? " [mem+busmaster ON]"
                                           : " [WARNING: bus master NOT enabled]");
    serial_puts("\n");

    if (!(cmd & PCI_CMD_MEM_SPACE) || !(cmd & PCI_CMD_BUS_MASTER))
        serial_puts("[SRCore] cannot access config space? AHCI will likely fail\n");

    return bar5;
}

uint32_t pci_scan(void) {
    uint32_t abar = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            /* header type 的 bit7 表示这是多功能设备 */
            uint32_t hdr = pci_read_config_dword((uint8_t)bus, slot, 0,
                                                 PCI_CFG_HEADER_TYPE);
            uint8_t nfunc = (hdr & 0x00800000u) ? 8 : 1;

            for (uint8_t func = 0; func < nfunc; func++) {
                uint32_t got = pci_check_func((uint8_t)bus, slot, func);
                if (got && !abar) abar = got;     /* 记住第一个控制器 */
            }
        }
    }

    if (!abar)
        serial_puts("[SRCore] no AHCI controller found\n");
    return abar;
}
