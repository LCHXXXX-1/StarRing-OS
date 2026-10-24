#ifndef AHCI_H
#define AHCI_H
#include <stdint.h>
#include <stdbool.h>

/* ============================================================
 *  AHCI (Advanced Host Controller Interface) 驱动
 *  纯轮询 + DMA，无 malloc：所有 DMA 缓冲区为全局静态数组。
 * ============================================================ */

/* ------------------ HBA（Generic Host Control）寄存器偏移 ------------------ */
#define HBA_CAP       0x00   /* Host Capabilities            */
#define HBA_GHC       0x04   /* Global Host Control          */
#define HBA_IS        0x08   /* Interrupt Status             */
#define HBA_PI        0x0C   /* Ports Implemented            */
#define HBA_VS        0x10   /* Version                      */
#define HBA_CCC_CTL   0x14   /* Command Completion Coalescing Control */
#define HBA_CCC_PORTS 0x18   /* Command Completion Coalescing Ports   */
#define HBA_EM_LOC    0x1C   /* Enclosure Management Location         */
#define HBA_EM_CTL    0x20   /* Enclosure Management Control          */
#define HBA_CAP2      0x24   /* Host Capabilities Extended            */
#define HBA_BOHC      0x28   /* BIOS/OS Handoff Control and Status    */

/* HBA_GHC 位 */
#define HBA_GHC_HR    (1u << 0)    /* HBA Reset                  */
#define HBA_GHC_IE    (1u << 1)    /* Interrupt Enable (全局)    */
#define HBA_GHC_AE    (1u << 31)   /* AHCI Enable                */

/* ------------------ Port 寄存器（相对端口基址） ------------------ */
#define PORT_BASE     0x100u       /* 端口寄存器区起始偏移       */
#define PORT_SIZE     0x80u        /* 每个端口 128 字节          */

#define PORTP_CLB     0x00         /* Command List Base Address (低) */
#define PORTP_CLBU    0x04         /* Command List Base Address (高) */
#define PORTP_FB      0x08         /* FIS Base Address (低)          */
#define PORTP_FBU     0x0C         /* FIS Base Address (高)          */
#define PORTP_IS      0x10         /* Interrupt Status               */
#define PORTP_IE      0x14         /* Interrupt Enable               */
#define PORTP_CMD     0x18         /* Command and Status             */
#define PORTP_RSV0    0x1C         /* reserved                       */
#define PORTP_TFD     0x20         /* Task File Data                 */
#define PORTP_SIG     0x24         /* Signature                      */
#define PORTP_SSTS    0x28         /* SATA Status (SCR0: SStatus)    */
#define PORTP_SCTL    0x2C         /* SATA Control (SCR2: SControl)  */
#define PORTP_SERR    0x30         /* SATA Error  (SCR1: SError)     */
#define PORTP_SACT    0x34         /* SATA Active (SCR3: SActive)    */
#define PORTP_CI      0x38         /* Command Issue                  */
#define PORTP_SNTF    0x3C         /* SATA Notification              */

/* PORTP_CMD 位 */
#define PORT_CMD_ST   (1u << 0)    /* Start (命令引擎运行)         */
#define PORT_CMD_SUD  (1u << 1)    /* Spin-Up Device               */
#define PORT_CMD_POD  (1u << 2)    /* Power On Device              */
#define PORT_CMD_CLO  (1u << 3)    /* Command List Override        */
#define PORT_CMD_FRE  (1u << 4)    /* FIS Receive Enable           */
#define PORT_CMD_FR   (1u << 14)   /* FIS Receive Running          */
#define PORT_CMD_CR   (1u << 15)   /* Command List Running         */

/* PORTP_TFD 位 */
#define TFD_STS_ERR   (1u << 0)
#define TFD_STS_DRQ   (1u << 3)
#define TFD_STS_DF    (1u << 5)
#define TFD_STS_BSY   (1u << 7)

/* PORTP_SSTS 位域 */
#define SSTS_DET_MASK      0x0Fu
#define SSTS_DET_PRESENT   0x03u   /* 设备已连接且 PHY 已建立 */
#define SSTS_IPM_MASK      0xF00u
#define SSTS_IPM_ACTIVE    0x100u
#define SSTS_POD           (1u << 8)  /* Power On Device：盘片/电机已上电 */

/* 设备签名 */
#define SATA_SIG_ATA       0x00000101u
#define SATA_SIG_ATAPI     0xEB140101u

/* ------------------ Command FIS / 命令表 ------------------ */
#define FIS_TYPE_REG_H2D   0x27u   /* Register FIS: Host to Device */
#define FIS_H2D_C          (1u << 7)   /* FIS byte1 bit7：更新命令寄存器 */

/* ATA 命令 */
#define ATA_CMD_READ_DMA_EX   0x25
#define ATA_CMD_WRITE_DMA_EX  0x35
#define ATA_CMD_IDENTIFY      0xEC
#define ATA_CMD_FLUSH_EXT     0xEA

#define AHCI_MAX_PRDT      8       /* 命令表内 PRDT 条目数 */

/* 单条 ATA 命令一次能连续搬运的扇区上限。
   单 PRDT 条目的 dbc 是 22 位（最大 4 MiB-1），这里保守取 1 MiB
   （2048 扇区），既远低于 dbc 上限，也不会触碰 16 位 count 的回绕。 */
#define AHCI_MAX_SECTORS_PER_CMD 2048u

/* Register Host-to-Device FIS（20 字节） */
struct ahci_fis_h2d {
    uint8_t  fis_type;             /* FIS_TYPE_REG_H2D */
    uint8_t  pmport;               /* bits0-3 PM port, bit7 = C */
    uint8_t  command;
    uint8_t  featurel;
    uint8_t  lba0;
    uint8_t  lba1;
    uint8_t  lba2;
    uint8_t  device;
    uint8_t  lba3;
    uint8_t  lba4;
    uint8_t  lba5;
    uint8_t  featureh;
    uint8_t  countl;
    uint8_t  counth;
    uint8_t  icc;
    uint8_t  control;
    uint32_t rsv1;
} __attribute__((packed));

/* PRDT（Physical Region Descriptor Table）条目（16 字节） */
struct ahci_prdt_entry {
    uint32_t dba;                  /* Data Base Address (低) */
    uint32_t dbau;                 /* Data Base Address (高) */
    uint32_t rsv0;
    uint32_t dbc;                  /* bits0-21 字节数-1, bit31 完成中断 */
} __attribute__((packed));

/* Command Header（32 字节，命令表 32 项）
   flags 位域：
     [4:0]  cfl  = Command FIS 长度（dword 数）
     [5]    a    = ATAPI
     [6]    w    = 写方向（1=Host->Device）
     [7]    p    = Prefetch
   其余位在非 BIST/非 PMP 场景保持 0。 */
#define CMD_HDR_CFL(len)  ((uint16_t)((len) & 0x1Fu))
#define CMD_HDR_WRITE     (1u << 6)

struct ahci_cmd_header {
    uint16_t flags;                /* cfl + 方向位 */
    uint16_t prdtl;                /* PRDT 条目数 */
    uint32_t prdbc;                /* 已传输字节数（由 HBA 回填） */
    uint32_t ctba;                 /* Command Table Base Address (低) */
    uint32_t ctbau;                /* Command Table Base Address (高) */
    uint32_t rsv1[4];
} __attribute__((packed));

/* Command Table（128 字节头 + PRDT） */
struct ahci_cmd_table {
    uint8_t                 cfis[64];   /* Command FIS */
    uint8_t                 acmd[16];   /* ATAPI command */
    uint8_t                 rsv[48];
    struct ahci_prdt_entry  prdt[AHCI_MAX_PRDT];
} __attribute__((aligned(128)));

/* ------------------ 对外接口 ------------------ */
bool     ahci_init(uint32_t abar);          /* 初始化控制器并识别所有硬盘 */
bool     ahci_present(void);

/* ---- 多盘接口（disk_idx = 0 .. ahci_disk_count()-1） ------------------- */
int      ahci_disk_count(void);
uint64_t ahci_disk_sectors(int disk_idx);   /* 指定硬盘总扇区数 */

/* 兼容旧单盘调用：默认 disk0。 */
uint64_t ahci_sectors(void);
bool     ahci_read_sector(uint64_t lba, void *buf);
bool     ahci_write_sector(uint64_t lba, const void *buf);

/* ---- 多扇区 DMA ---------------------------------------------------------
   一次命令连续搬运 count 个 512 字节扇区：
     - FIS 的 count 字段 = 本次命令的扇区数（可 >1）；
     - PRDT 条目 dbc = 扇区数*512 - 1；
   驱动内部统一经 4KB 对齐的静态 bounce buffer 搬运，所以调用方缓冲区
   无需 4KB 对齐、也可以是栈地址；按 bounce 缓冲上限分段（每段不超过
   AHCI_MAX_SECTORS_PER_CMD）；lba+count 越过该盘容量一律拒绝。 */
bool     ahci_read_sectors (int disk_idx, uint64_t lba, uint32_t count,
                            void *buf);
bool     ahci_write_sectors(int disk_idx, uint64_t lba, uint32_t count,
                            const void *buf);

bool     ahci_flush_disk(int disk_idx);     /* 刷写指定盘写缓存 */
bool     ahci_flush(void);                  /* 兼容：刷 disk0 */

/* 累计搬运成功的扇区数（任务管理器 I/O 统计用；不做清空） */
extern volatile uint64_t read_sectors_total;
extern volatile uint64_t write_sectors_total;

#endif
