#ifndef PCI_H
#define PCI_H
#include <stdint.h>
#include <stdbool.h>

/* PCI 配置空间访问（legacy 0xCF8/0xCFC 端口机制） */
#define PCI_CONFIG_ADDRESS 0xCF8u
#define PCI_CONFIG_DATA    0xCFCu

/* 配置空间常见偏移 */
#define PCI_CFG_VENDOR_ID   0x00
#define PCI_CFG_DEVICE_ID   0x02
#define PCI_CFG_COMMAND     0x04
#define PCI_CFG_STATUS      0x06
#define PCI_CFG_REVISION    0x08
#define PCI_CFG_PROG_IF     0x09
#define PCI_CFG_SUBCLASS    0x0A
#define PCI_CFG_CLASS       0x0B
#define PCI_CFG_HEADER_TYPE 0x0E
#define PCI_CFG_BAR0        0x10
#define PCI_CFG_BAR5        0x24

/* 类代码：大容量存储控制器 / SATA / AHCI 1.0 */
#define PCI_CLASS_STORAGE   0x01
#define PCI_SUBCLASS_SATA   0x06
#define PCI_PROGIF_AHCI     0x01

/* PCI_CFG_COMMAND 位 */
#define PCI_CMD_IO_SPACE     (1u << 0)  /* I/O 空间译码            */
#define PCI_CMD_MEM_SPACE    (1u << 1)  /* 内存空间译码（MMIO/ABAR）*/
#define PCI_CMD_BUS_MASTER   (1u << 2)  /* 总线主控（DMA 必需）     */

struct pci_dev {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor;
    uint16_t device;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
};

uint32_t pci_read_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint16_t pci_read_config_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void     pci_write_config_dword(uint8_t bus, uint8_t slot, uint8_t func,
                                uint8_t off, uint32_t val);
void     pci_write_config_word(uint8_t bus, uint8_t slot, uint8_t func,
                               uint8_t off, uint16_t val);

/* 遍历 PCI 总线，寻找 AHCI 控制器。
   找到时打印 Vendor ID / Device ID / ABAR，返回其 BAR5（ABAR 物理基址）；
   未找到返回 0。 */
uint32_t pci_scan(void);

#endif
