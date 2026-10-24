#include "ahci.h"
#include "arch/x86_64/cpu.h"
#include "drivers/serial.h"
#include "lib/mem.h"
#include "mm/vmm.h"
#include "boot/limine_requests.h"
#include <limine.h>

/* ------------------------------------------------------------
   Limine 提供两个地址翻译信息：
     - HHDM：物理 RAM（不含 MMIO 空洞）线性映射到内核虚拟空间，
       用于访问页表自身；
     - Kernel Address：内核镜像的物理/虚拟基址，用于把 .bss 里的
       DMA 缓冲区换算回物理地址。
   若上述信息缺失（内核未开启分页），则全部退化为“直接强转物理地址”。

   注意：所有 Limine 请求只在 boot/limine_requests.c 定义一次
   （重复的 request ID 会让 Limine 启动时直接 PANIC）。
   HHDM 通过 vmm_hhdm_offset() 取用，KERNEL_ADDRESS 用全局 g_kaddr_req。
   ------------------------------------------------------------ */

static uint64_t g_hhdm  = 0;   /* 物理 → 虚拟 的直接映射偏移 */
static uint64_t g_kvirt = 0;   /* 内核镜像虚拟基址 */
static uint64_t g_kphys = 0;   /* 内核镜像物理基址 */

static void resolve_addressing(void) {
    g_hhdm = vmm_hhdm_offset();
    if (g_kaddr_req.response) {
        g_kvirt = g_kaddr_req.response->virtual_base;
        g_kphys = g_kaddr_req.response->physical_base;
    }
    serial_puts("[SRCore] hhdm=");
    serial_puthex(g_hhdm);
    serial_puts(" kphys=");
    serial_puthex(g_kphys);
    serial_puts("\n");
}

/* ============================================================
 *  静态 DMA 缓冲区（禁止 malloc，全部放 .bss）
 *  - 命令列表：1KB 对齐（规范要求），这里给足 4KB
 *  - FIS 接收缓冲：256 字节对齐
 *  - 命令表：128 字节对齐
 * ============================================================ */
#define AHCI_MAX_DISKS 32

/* 每块盘独立一套命令列表 / FIS 接收缓冲 / 命令表。虽然本内核是纯轮询、
   同一时刻只下发一条命令，但让每个端口绑定自己的 DMA 结构可以避免端口
   重启或设备异步 FIS 时互相踩内存。 */
static struct ahci_cmd_header g_cmd_list[AHCI_MAX_DISKS][32]
    __attribute__((aligned(4096)));
static uint8_t               g_fis_buf[AHCI_MAX_DISKS][256]
    __attribute__((aligned(4096)));
static struct ahci_cmd_table g_cmd_tbl[AHCI_MAX_DISKS]
    __attribute__((aligned(4096)));
static uint16_t              g_ident[256]    __attribute__((aligned(4096)));

/* 阶段一：PRDT 指向的数据缓冲必须是 4KB 对齐的静态数组。
   调用方（SRFS 块缓存 / shell 栈缓冲 / 未来的 FAT32 簇缓冲）地址不可控，
   因此多扇区搬运统一经这个对齐的静态 bounce buffer：
     写：memcpy 进 g_bounce → DMA(Host→Device)；
     读：DMA(Device→Host) → memcpy 出 g_bounce。 */
#define AHCI_BOUNCE_SECTORS 128u            /* 64 KiB，足够摊薄 memcpy 开销 */
_Static_assert(AHCI_BOUNCE_SECTORS <= AHCI_MAX_SECTORS_PER_CMD,
               "bounce chunk exceeds single-command limit");
static uint8_t g_bounce[AHCI_BOUNCE_SECTORS * 512u]
    __attribute__((aligned(4096)));

_Static_assert(sizeof(struct ahci_fis_h2d)    == 20, "FIS size");
_Static_assert(sizeof(struct ahci_prdt_entry) == 16, "PRDT entry size");
_Static_assert(sizeof(struct ahci_cmd_header) == 32, "Command header size");

/* ---- 运行时状态 ---- */
static volatile uint8_t *g_hba = 0;          /* ABAR 映射后的虚拟/物理指针 */
static int      g_ports[AHCI_MAX_DISKS];     /* disk_idx → HBA 端口号 */
static uint64_t g_disk_sectors[AHCI_MAX_DISKS];
static int      g_disk_count = 0;
static bool     g_ready      = false;

/* 累计成功搬运的扇区数（只增不减，任务管理器读取） */
volatile uint64_t read_sectors_total  = 0;
volatile uint64_t write_sectors_total = 0;

/* HBA 是否会把实际搬运的字节数回填到 Command Header 的 PRDBC 字段。
   用 IDENTIFY 校准：它能读到真实容量就说明该命令确实执行完 DMA；
     g_prdbc_ok = 1 → 控制器实现了 PRDBC，之后每条 DMA 命令都严格校验
                      “搬运字节数 == 预期”，数据没搬完即判失败重试；
     g_prdbc_ok = 0 → 控制器不回填（恒为 0），只能靠 PxCI 退休 + TFD/SERR
                      判定成败，避免因 emulation 差异把好命令误判为失败。 */
static int g_prdbc_ok = -1;

/* ---- 寄存器访问 ---- */
static inline uint32_t hba_rd(uint32_t off) {
    return *(volatile uint32_t *)(g_hba + off);
}
static inline void hba_wr(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_hba + off) = v;
}
static inline uint32_t p_reg(int p, uint32_t off) {
    return PORT_BASE + (uint32_t)p * PORT_SIZE + off;
}
static inline uint32_t p_rd(int p, uint32_t off) {
    return *(volatile uint32_t *)(g_hba + p_reg(p, off));
}
static inline void p_wr(int p, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_hba + p_reg(p, off)) = v;
}

/* ---- 虚拟地址 → 物理地址 ----
   AHCI 的 DMA 需要真实物理地址，这里有三种指针来源：
     1) 内核镜像（higher-half，DMA 缓冲都在 .bss）：
        phys = kphys + (virt - kvirt)
     2) Limine HHDM 线性映射的物理 RAM：phys = virt - hhdm
     3) 未启用分页（kvirt/hhdm 均为 0）：退化为直接强转 ---- */
static inline uint64_t virt_to_phys64(const void *p) {
    uint64_t v = (uint64_t)(uintptr_t)p;
    if (g_kvirt && v >= g_kvirt) return g_kphys + (v - g_kvirt);  /* 内核镜像 */
    if (g_hhdm  && v >= g_hhdm)  return v - g_hhdm;               /* HHDM 映射 */
    return v;                                                     /* 无分页   */
}
static inline uint32_t virt_to_phys(const void *p) {
    return (uint32_t)virt_to_phys64(p);   /* DMA 缓冲位于低 4GB，直接截断安全 */
}
static inline uint32_t phys_lo(const void *p) { return virt_to_phys(p); }
static inline uint32_t phys_hi(const void *p) {
    return (uint32_t)(virt_to_phys64(p) >> 32);   /* DMA 缓冲区低于 4GB，通常为 0 */
}

/* ============================================================
 *  MMIO 映射
 *  Limine 的 HHDM 只覆盖物理 RAM，AHCI 的 ABAR 位于 MMIO 空洞
 *  （如 0xFEBB1000），因此这里手动在页表中建立一段 2MB 大页映射。
 *  页表放在 .bss 的静态数组里（无 malloc）。
 * ============================================================ */
#define MMIO_VA      0xFFFFFFFFC0000000ULL   /* 借用的内核虚拟地址 */
#define PTE_P        0x001ULL                /* Present            */
#define PTE_RW       0x002ULL                /* Read/Write         */
#define PTE_PWT      0x008ULL                /* Write-Through      */
#define PTE_PCD      0x010ULL                /* Cache Disable (UC) */
#define PTE_PS       0x080ULL                /* Page Size (2MB)    */

static uint64_t g_pdpt[512] __attribute__((aligned(4096)));
static uint64_t g_pd[512]   __attribute__((aligned(4096)));

/* 用 HHDM 把页表项的物理地址转成可访问的虚拟地址 */
static inline uint64_t *pt_at(uint64_t paddr) {
    return (uint64_t *)(uintptr_t)(g_hhdm + (paddr & ~0xFFFULL));
}

/* 把物理地址 phys 所在的 2MB 区映射到虚拟地址 va（可写、非缓存） */
static void map_mmio_2mb(uint64_t va, uint64_t phys) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));

    uint64_t *pml4 = pt_at(cr3);
    uint64_t i4 = (va >> 39) & 0x1FF;
    if (!(pml4[i4] & PTE_P)) {
        memset(g_pdpt, 0, sizeof(g_pdpt));
        pml4[i4] = virt_to_phys64(g_pdpt) | PTE_P | PTE_RW;
    }
    uint64_t *pdpt = pt_at(pml4[i4]);
    uint64_t i3 = (va >> 30) & 0x1FF;
    if (!(pdpt[i3] & PTE_P)) {
        memset(g_pd, 0, sizeof(g_pd));
        pdpt[i3] = virt_to_phys64(g_pd) | PTE_P | PTE_RW;
    }
    uint64_t *pd = pt_at(pdpt[i3]);
    uint64_t i2 = (va >> 21) & 0x1FF;
    pd[i2] = (phys & ~0x1FFFFFULL) | PTE_P | PTE_RW | PTE_PWT | PTE_PCD | PTE_PS;

    __asm__ volatile ("invlpg (%0)" :: "r"((void *)(uintptr_t)va) : "memory");
}

/* 返回 ABAR 起始处可访问的虚拟地址 */
static void *map_abar(uint32_t abar) {
    uint64_t base = (uint64_t)abar & ~0x1FFFFFULL;   /* 2MB 对齐 */
    if (!g_hhdm) return (void *)(uintptr_t)abar;     /* 无分页：直接强转 */
    map_mmio_2mb(MMIO_VA, base);
    return (void *)(uintptr_t)(MMIO_VA + ((uint64_t)abar - base));
}

/* ---- 轮询等待 ----
   所有轮询都有上限，绝不无限自旋，避免内核永久死机。 */
#define AHCI_POLL_MAX        1000000u    /* 空闲/读命令：正常很快完成 */
#define AHCI_WRITE_POLL_MAX  4000000u    /* 写命令：慢盘/虚拟机余量更大 */
#define AHCI_FLUSH_POLL_MAX  40000000u   /* FLUSH 会触发宿主机 fsync，给足时间 */
#define AHCI_CMD_RETRY       3u          /* 单条命令最多尝试次数（含首次） */

/* 置 1 可打印每条 AHCI 命令（调试用；正式运行保持 0，避免串口刷屏） */
#define AHCI_DEBUG     0

/* ---- 内存屏障（写盘超时最关键的修复点）----
   命令列表 / 命令表 / PRDT 都在普通 WB 内存里，而 PxCI、PxCMD 是 UC MMIO。
   x86 允许“对 UC 的写”越过“对 WB 的写”先生效，于是 HBA 可能取到还没写完
   的命令表（cfl=0、PRDT 全零）——命令永不退休，表现为 [SRCore] command
   timeout；或数据只搬了一半，表现为写下去的内容是垃圾。
     - 下发命令前必须 sfence（保证内存写先于 MMIO 写可见）
     - 读取 HBA 回填的 PRDBC / 状态前必须 lfence（保证不被提前读到旧值） */
static inline void ahci_sfence(void) { __asm__ volatile ("sfence" ::: "memory"); }
static inline void ahci_lfence(void) { __asm__ volatile ("lfence" ::: "memory"); }

static bool wait_clear(int p, uint32_t off, uint32_t mask) {
    for (uint32_t i = 0; i < AHCI_POLL_MAX; i++)
        if (!(p_rd(p, off) & mask)) return true;
    return false;
}
static bool wait_set(int p, uint32_t off, uint32_t mask) {
    for (uint32_t i = 0; i < AHCI_POLL_MAX; i++)
        if (p_rd(p, off) & mask) return true;
    return false;
}

/* 出错时清掉端口错误/中断状态，尽量让端口回到可用状态，
   避免一次失败后后续命令全部跟着卡住。 */
static void port_clear_error(int p) {
    p_wr(p, PORTP_SERR, 0xFFFFFFFFu);
    p_wr(p, PORTP_IS,   0xFFFFFFFFu);
}

/* 命令超时后中止尚未完成的命令（置 CLO，等硬件清掉 CI），
   让端口恢复到可继续下发命令的状态，而不是永久卡死。 */
static void port_abort_cmd(int p) {
    uint32_t cmd = p_rd(p, PORTP_CMD);
    p_wr(p, PORTP_CMD, cmd | PORT_CMD_CLO);
    wait_clear(p, PORTP_CMD, PORT_CMD_CLO);
    port_clear_error(p);
}

/* 调试用：打印端口的 TFD / CI / IS / SERR / SACT 状态 */
static void port_dump(int p) {
    serial_puts(" tfd=");  serial_puthex(p_rd(p, PORTP_TFD));
    serial_puts(" ci=");   serial_puthex(p_rd(p, PORTP_CI));
    serial_puts(" is=");   serial_puthex(p_rd(p, PORTP_IS));
    serial_puts(" serr="); serial_puthex(p_rd(p, PORTP_SERR));
    serial_puts(" sact="); serial_puthex(p_rd(p, PORTP_SACT));
    serial_puts(" cmd=");  serial_puthex(p_rd(p, PORTP_CMD));
}

/* 等端口空闲，三个条件缺一不可才能下发新命令：
     1) PxTFD 的 BSY/DRQ = 0   —— 设备不忙、数据期已结束
     2) PxCI   bit(slot0) = 0  —— 上一条命令已从命令列表退休
     3) PxSACT bit(slot0) = 0  —— 上一条命令的 Activate  FIS 已经收到
   第 3 条是以前漏掉的：SACT 还挂着时写 PxCI 会被 HBA 直接忽略，
   于是“命令根本没执行 → CI 一直是 0 → 误判成功/或下一步卡死”。
   TFD 里的 ERR/DF 是“上一条命令”遗留的粘滞位：只要端口本身已经空闲，
   就打印告警、清掉 SERR 后继续下发新命令（设备会在新命令开始时刷新状态），
   绝不能因为一个遗留错误位就把后续所有命令永久判死。 */
static bool wait_port_idle(int p, bool write) {
    for (uint32_t i = 0; i < AHCI_POLL_MAX; i++) {
        uint32_t tfd = p_rd(p, PORTP_TFD);
        if (tfd & (TFD_STS_BSY | TFD_STS_DRQ)) continue;
        if (p_rd(p, PORTP_CI)   & 1u) continue;
        if (p_rd(p, PORTP_SACT) & 1u) continue;
        if (tfd & (TFD_STS_ERR | TFD_STS_DF)) {
            serial_puts(write ? "[SRCore] write: stale tfd=" : "[SRCore] cmd: stale tfd=");
            serial_puthex(tfd);
            serial_puts(" serr="); serial_puthex(p_rd(p, PORTP_SERR));
            serial_puts(" (cleared, continuing)\n");
            port_clear_error(p);
        }
        return true;
    }
    serial_puts(write ? "[SRCore] write timeout" : "[SRCore] command timeout");
    serial_puts(" (port busy before command):");
    port_dump(p);
    serial_puts("\n");
    port_abort_cmd(p);
    return false;
}

/* ---- 停止 / 启动端口 ---- */
static void port_stop(int p) {
    uint32_t cmd = p_rd(p, PORTP_CMD);
    cmd &= ~PORT_CMD_ST;
    p_wr(p, PORTP_CMD, cmd);
    wait_clear(p, PORTP_CMD, PORT_CMD_CR);

    cmd = p_rd(p, PORTP_CMD);
    cmd &= ~PORT_CMD_FRE;
    p_wr(p, PORTP_CMD, cmd);
    wait_clear(p, PORTP_CMD, PORT_CMD_FR);
}

static bool port_start(int p, int disk_idx) {
    /* 绑定该盘自己的命令列表 / FIS 缓冲区（DMA 基址）
       注意：必须在 FRE=0 时写基址，写完要有屏障，再启动引擎。 */
    struct ahci_cmd_header *cl = g_cmd_list[disk_idx];
    uint8_t               *fb = g_fis_buf[disk_idx];
    p_wr(p, PORTP_CLB,  phys_lo(cl));
    p_wr(p, PORTP_CLBU, phys_hi(cl));
    p_wr(p, PORTP_FB,   phys_lo(fb));
    p_wr(p, PORTP_FBU,  phys_hi(fb));
    ahci_sfence();

    p_wr(p, PORTP_IS,   0xFFFFFFFFu);          /* 清中断状态（W1C） */
    p_wr(p, PORTP_SERR, 0xFFFFFFFFu);          /* 清错误（W1C） */
    p_wr(p, PORTP_IE,   0x00000000u);          /* 关闭该端口中断（纯轮询） */

    uint32_t cmd = p_rd(p, PORTP_CMD);
    cmd |= PORT_CMD_FRE;
    p_wr(p, PORTP_CMD, cmd);
    if (!wait_set(p, PORTP_CMD, PORT_CMD_FR)) return false;

    /* 顺手要求设备上电（SUD）。空盘/刚接线的盘若没 spin up，第一条命令
       会一直挂着直到超时，这是“全新硬盘第一次写就 timeout”的常见原因。 */
    cmd = p_rd(p, PORTP_CMD);
    cmd |= PORT_CMD_SUD;
    p_wr(p, PORTP_CMD, cmd);
    for (uint32_t i = 0; i < AHCI_POLL_MAX && !(p_rd(p, PORTP_SSTS) & SSTS_POD); i++)
        ;

    cmd = p_rd(p, PORTP_CMD);
    cmd |= PORT_CMD_ST;
    p_wr(p, PORTP_CMD, cmd);
    if (!wait_set(p, PORTP_CMD, PORT_CMD_CR)) return false;
    return true;
}

/* 命令失败后的端口恢复，逐级加重，每级都有超时上限：
     level 1：CLO 放弃挂起命令 + 清 PIS/SERR（轻，绝大多数瞬断够用）
     level ≥2：停端口 → 重绑命令列表/FIS 基址 → 启端口（把引擎彻底重启）
   刻意不做 COMRESET 链路复位：VMware/实体盘一旦复位失败，整块盘就消失了，
   比放弃这一次写操作严重得多，宁可返回 false 让上层报错。 */
static bool port_recover(int disk_idx, uint32_t level) {
    if (disk_idx < 0 || disk_idx >= g_disk_count) return false;
    int p = g_ports[disk_idx];
    if (level <= 1) {
        port_abort_cmd(p);
        return true;
    }
    serial_puts("[SRCore] restarting disk ");
    serial_putdec((uint64_t)disk_idx);
    serial_puts(" port ");
    serial_putdec((uint64_t)p);
    serial_puts("\n");
    port_stop(p);
    port_clear_error(p);
    if (!port_start(p, disk_idx)) {
        serial_puts("[SRCore] port restart failed\n");
        return false;
    }
    return true;
}

/* ============================================================
 *  通用命令执行（纯轮询，固定用 slot 0）
 *
 *  这一版把写盘超时的几个根因全部堵上：
 *   1) 命令表/命令列表是 WB 内存，PxCI 是 UC MMIO，两者之间没有屏障 →
 *      HBA 取到半成品命令表，命令永不退休（[SRCore] command timeout）。
 *      现在下发前 sfence，写完 PxCI 再读回强制 posted write 落地。
 *   2) 只等 BSY/DRQ 就下发，没等 PxSACT 清零 → 命令被 HBA 忽略。
 *      现在 CI/SACT 都必须为 0（见 wait_port_idle）。
 *   3) 完成判定拿 TFD 的 ERR/DF 当“提前退出”条件 → 那是上一条命令的
 *      粘滞位，刚下发就会“秒退”，然后把遗留错误当成本次结果，一错到底。
 *      现在只认 PxCI 清零；错误一律等命令退休之后再判定。
 *   4) 不校验命令是否被真正接受、数据是否搬完 → 现在下发前严格校验
 *      Command Header 的 W 位/CFL/命令表基址、FIS 的 C 位/类型/操作码、
 *      PRDT 的地址与字节数；完成后校验 HBA 回填的 PRDBC == 预期字节数。
 *   5) 一次失败就整体放弃 → 现在带端口恢复的重试（AHCI_CMD_RETRY 次），
 *      全部失败也只返回 false，任何路径都有超时上限，绝不挂死。
 * ============================================================ */

/* 单次下发：构造命令 → 严格校验 → 提交 → 轮询退休 → 校验搬运结果。
   返回  1 = 成功；0 = 失败（可重试）；-1 = 描述符构造错误（重试也没用）。
   调用前必须已确认端口空闲（wait_port_idle）。 */
static int ahci_exec_once(int disk_idx, uint8_t ata_cmd, uint64_t lba,
                          uint16_t count, void *buf, uint32_t bytes,
                          bool write, uint32_t poll_max) {
    if (disk_idx < 0 || disk_idx >= g_disk_count) return -1;
    int p = g_ports[disk_idx];
    struct ahci_cmd_header *cmd_list = g_cmd_list[disk_idx];
    struct ahci_cmd_table  *cmd_tbl  = &g_cmd_tbl[disk_idx];
    const uint16_t cfl_dw = (uint16_t)(sizeof(struct ahci_fis_h2d) / 4); /* = 5 */

#if AHCI_DEBUG
    serial_puts("[SRCore] cmd="); serial_puthex(ata_cmd);
    serial_puts(" lba=");       serial_puthex(lba);
    serial_puts(" cnt=");       serial_puthex(count);
    serial_puts(write ? " dir=W\n" : " dir=R\n");
#endif

    memset(cmd_list, 0, sizeof(g_cmd_list[disk_idx]));
    memset(cmd_tbl, 0, sizeof(*cmd_tbl));

    struct ahci_cmd_header *hdr = &cmd_list[0];
    /* Command Header.flags：[4:0]=cfl（Command FIS 长度，dword 数）；
       bit6 = W（写方向位）。写操作时 W 必须置 1（Host->Device），
       否则 HBA 不会把 PRDT 数据发给设备，设备会一直等数据直到超时。 */
    hdr->flags = (uint16_t)(CMD_HDR_CFL(sizeof(struct ahci_fis_h2d) / 4) |
                            (write ? CMD_HDR_WRITE : 0));
    hdr->prdtl = (bytes == 0) ? 0 : 1;       /* 传输长度：1 个 PRDT 条目 */
    hdr->ctba  = phys_lo(cmd_tbl);
    hdr->ctbau = phys_hi(cmd_tbl);

    /* 2) 构造 Register H2D FIS（20 字节）：
         byte0 = 0x27 (FIS Type)
         byte1 = bit7 C + [4:0] PM Port     —— C 位（Update Command Register）
         byte2 = 命令码（原始 ATA 命令，写为 0x35，不得改写）
       C 位必须置 1，否则设备收到 FIS 后不更新命令寄存器、命令根本不执行，
       PxCI 永不清零 → 必然 command timeout。
       注意：C 位在 byte1（本结构的 pmport 字段），不在 command 字段；
       把 command 写成 0x35|0x80=0xB5 是非法 ATA 操作码，设备会直接 ABRT。 */
    struct ahci_fis_h2d *fis = (struct ahci_fis_h2d *)cmd_tbl->cfis;
    memset((void *)fis, 0, sizeof(*fis));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport   = (uint8_t)FIS_H2D_C;      /* C 位 = 1：更新命令寄存器 */
    fis->command  = ata_cmd;                 /* 原始命令码：0x25/0x35/0xEC/0xEA */
    fis->device   = 0x40;                    /* LBA 模式 */
    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8)  & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);
    /* 阶段一：count 可 >1 —— FIS 的 count 字段即多扇区一次搬运的扇区数 */
    fis->countl = (uint8_t)(count & 0xFF);
    fis->counth = (uint8_t)((count >> 8) & 0xFF);

    /* 3) PRDT（单条目；dbc = 本次命令的总字节数-1 = count*512-1） */
    if (bytes) {
        struct ahci_prdt_entry *prdt = &cmd_tbl->prdt[0];
        prdt->dba  = phys_lo(buf);
        prdt->dbau = phys_hi(buf);
        prdt->dbc  = (bytes - 1) & 0x3FFFFFu;
    }

    /* ---- 4) 严格校验：HBA / 设备读到的就是内存里的这些字节 ----
       刻意“再从内存读一遍”而不是相信自己刚写进去的寄存器值：
       结构体布局、对齐、方向位一旦被改坏，这里立刻报出来，
       而不是等到超时才猜。 */
    volatile uint8_t *cf  = (volatile uint8_t *)cmd_tbl->cfis;  /* FIS 原始字节 */
    uint16_t  got_flags   = hdr->flags;
    uint16_t  want_flags  = (uint16_t)(CMD_HDR_CFL(cfl_dw) |
                                       (write ? CMD_HDR_WRITE : 0));
    uint16_t  got_prdtl   = hdr->prdtl;
    uint32_t  got_ctba    = hdr->ctba;
    uint8_t   got_type    = cf[0];
    uint8_t   got_c       = cf[1];
    uint8_t   got_cmd     = cf[2];

    if ((got_flags & 0x1Fu) != cfl_dw) {
        serial_puts("[SRCore] BAD cfl: flags="); serial_puthex(got_flags);
        serial_puts(" want="); serial_puthex(want_flags); serial_puts("\n");
        return -1;
    }
    /* Command Header bit6 = W：写方向必须 1，读方向必须 0 */
    if (write && !(got_flags & CMD_HDR_WRITE)) {
        serial_puts("[SRCore] BAD header: WRITE but W(bit6)=0, flags=");
        serial_puthex(got_flags); serial_puts("\n");
        return -1;
    }
    if (!write && (got_flags & CMD_HDR_WRITE)) {
        serial_puts("[SRCore] BAD header: READ but W(bit6)=1, flags=");
        serial_puthex(got_flags); serial_puts("\n");
        return -1;
    }
    if (got_ctba != phys_lo(cmd_tbl)) {
        serial_puts("[SRCore] BAD ctba="); serial_puthex(got_ctba);
        serial_puts(" want="); serial_puthex(phys_lo(cmd_tbl));
        serial_puts("\n");
        return -1;
    }
    if (got_prdtl != (uint16_t)(bytes ? 1u : 0u)) {
        serial_puts("[SRCore] BAD prdtl="); serial_puthex(got_prdtl);
        serial_puts("\n");
        return -1;
    }
    /* Register H2D FIS：byte0 类型、byte1 bit7 = C（Update Command Register） */
    if (got_type != FIS_TYPE_REG_H2D) {
        serial_puts("[SRCore] BAD fis type="); serial_puthex(got_type);
        serial_puts("\n");
        return -1;
    }
    if (!(got_c & FIS_H2D_C)) {
        serial_puts("[SRCore] BAD fis: C(bit7)=0, byte1=");
        serial_puthex(got_c); serial_puts("\n");
        return -1;
    }
    if (got_cmd != ata_cmd) {
        serial_puts("[SRCore] BAD fis command="); serial_puthex(got_cmd);
        serial_puts(" want="); serial_puthex(ata_cmd); serial_puts("\n");
        return -1;
    }
    if (bytes) {
        struct ahci_prdt_entry *pv = &cmd_tbl->prdt[0];
        if (pv->dba != phys_lo(buf) || pv->dbc != ((bytes - 1) & 0x3FFFFFu)) {
            serial_puts("[SRCore] BAD prdt dba="); serial_puthex(pv->dba);
            serial_puts(" dbc="); serial_puthex(pv->dbc); serial_puts("\n");
            return -1;
        }
    }

    /* ---- 5) 提交命令 ----
       sfence：命令表/PRDT 的 WB 写入必须先于 PxCI 的 UC MMIO 生效；
       写完 PxCI 再读一次：MMIO 读会强制前面的 posted write 落地，
       避免下一行轮询读回的还是旧值。 */
    ahci_sfence();
    port_clear_error(p);
    p_wr(p, PORTP_CI, 1u << 0);
    (void)p_rd(p, PORTP_CI);

    /* ---- 6) 等命令退休：只认 PxCI 被控制器清零 ----
       不再用 TFD 的 ERR/DF 提前退出（那是上一条命令的粘滞位，
       会让“刚下发”被误判成“已完成”，再把遗留错误当成本次结果）。 */
    bool     retired = false;
    uint32_t spins   = 0;
    for (; spins < poll_max; spins++) {
        if (!(p_rd(p, PORTP_CI) & 1u)) { retired = true; break; }
    }
    ahci_lfence();                 /* 之后读 hdr->prdbc 才是 HBA 回填的值 */
    uint32_t prdbc = hdr->prdbc;

    /* 用第一条 DMA 命令（IDENTIFY，它必然成功）校准本控制器是否回填 PRDBC */
    if (g_prdbc_ok < 0 && bytes)
        g_prdbc_ok = (prdbc == bytes) ? 1 : 0;

    if (!retired) {
        serial_puts(write ? "[SRCore] write timeout" : "[SRCore] command timeout");
        serial_puts(" cmd="); serial_puthex(ata_cmd);
        serial_puts(" lba="); serial_puthex(lba);
        serial_puts(" cnt="); serial_puthex(count);
        serial_puts(" spins="); serial_puthex(spins);
        serial_puts(" prdbc="); serial_puthex(prdbc);
        port_dump(p);
        serial_puts("\n");
        return 0;                   /* 超时：交给上层恢复 + 重试 */
    }

    /* ---- 7) 命令已退休：判定设备有没有报错 ---- */
    uint32_t tfd  = p_rd(p, PORTP_TFD);
    uint32_t serr = p_rd(p, PORTP_SERR);
    uint32_t sact = p_rd(p, PORTP_SACT);
    if (tfd & (TFD_STS_ERR | TFD_STS_DF)) {
        serial_puts(write ? "[SRCore] write error: cmd=" : "[SRCore] read error: cmd=");
        serial_puthex(ata_cmd);
        serial_puts(" lba=");   serial_puthex(lba);
        serial_puts(" tfd=");   serial_puthex(tfd);
        serial_puts(" serr=");  serial_puthex(serr);
        serial_puts(" prdbc="); serial_puthex(prdbc);
        serial_puts("\n");
        port_clear_error(p);
        return 0;
    }
    /* CI 已清零，SACT 一般同时清零；个别控制器有极短的传播窗口。
       先小量轮询等它清掉，仍未清就只告警——不据此判失败，否则会把
       其实已经写成功的命令误杀（下一次 wait_port_idle 还会再等它）。 */
    if (sact & 1u) {
        for (uint32_t i = 0; i < 2000u && (p_rd(p, PORTP_SACT) & 1u); i++)
            ;
        sact = p_rd(p, PORTP_SACT);
        if (sact & 1u) {
            serial_puts("[SRCore] warn: sact="); serial_puthex(sact);
            serial_puts(" still set after retire\n");
        }
    }

    /* ---- 8) 校验数据确实搬完（PRD Byte Count 由 HBA 回填） ---- */
    if (bytes && g_prdbc_ok == 1 && prdbc != bytes) {
        serial_puts(write ? "[SRCore] write DMA incomplete: prdbc="
                          : "[SRCore] read DMA incomplete: prdbc=");
        serial_puthex(prdbc);
        serial_puts(" want="); serial_puthex(bytes);
        serial_puts(" cmd=");  serial_puthex(ata_cmd);
        serial_puts(" lba=");  serial_puthex(lba);
        serial_puts("\n");
        return 0;
    }
    return 1;
}

/* 高层封装：等端口空闲 → 下发 → 失败则恢复端口并重试，共 AHCI_CMD_RETRY 次。
   写同一个扇区是幂等的，重试安全；所有等待都有上限，用尽后只返回 false。 */
static bool ahci_run_cmd(int disk_idx, uint8_t ata_cmd, uint64_t lba,
                         uint16_t count, void *buf, uint32_t bytes,
                         bool write, uint32_t poll_max) {
    if (disk_idx < 0 || disk_idx >= g_disk_count || !g_hba) return false;
    int p = g_ports[disk_idx];

    for (uint32_t attempt = 1; attempt <= AHCI_CMD_RETRY; attempt++) {
        if (!wait_port_idle(p, write)) {
            if (attempt >= AHCI_CMD_RETRY) break;
            port_recover(disk_idx, attempt);   /* 端口没空闲：恢复后重试 */
            continue;
        }
        int r = ahci_exec_once(disk_idx, ata_cmd, lba, count, buf, bytes,
                               write, poll_max);
        if (r == 1) return true;
        if (r < 0) break;                      /* 描述符写坏了，重试无意义 */
        if (attempt >= AHCI_CMD_RETRY) break;
        serial_puts("[SRCore] retry "); serial_putdec(attempt + 1);
        serial_puts("/"); serial_putdec(AHCI_CMD_RETRY);
        serial_puts(" disk="); serial_putdec((uint64_t)disk_idx);
        serial_puts(" cmd="); serial_puthex(ata_cmd);
        serial_puts(" lba="); serial_puthex(lba);
        serial_puts(write ? " (W)\n" : " (R)\n");
        port_recover(disk_idx, attempt);
    }
    /* 放弃之前先把端口清干净：挂起的 slot0 若留着，之后每条命令都要先白
       轮询满 AHCI_POLL_MAX 次才失败，整个文件系统的写操作会被拖死。 */
    port_recover(disk_idx, 1);
    serial_puts(write ? "[SRCore] write FAILED after " : "[SRCore] cmd FAILED after ");
    serial_putdec(AHCI_CMD_RETRY);
    serial_puts(" attempts: cmd="); serial_puthex(ata_cmd);
    serial_puts(" lba="); serial_puthex(lba);
    serial_puts(" bytes="); serial_putdec(bytes);
    serial_puts("\n");
    return false;
}

/* ---------- 对外接口 ---------- */

bool ahci_init(uint32_t abar) {
    g_hba = 0;
    g_disk_count = 0;
    g_ready = false;
    g_prdbc_ok = -1;          /* 重新校准：换控制器/重初始化都要重来 */
    for (int i = 0; i < AHCI_MAX_DISKS; i++) {
        g_ports[i] = -1;
        g_disk_sectors[i] = 0;
    }

    resolve_addressing();

    if (!abar) {
        serial_puts("[SRCore] invalid ABAR\n");
        return false;
    }
    /* ABAR 是物理地址；在页表中建立映射后访问 */
    g_hba = (volatile uint8_t *)map_abar(abar);
    serial_puts("[SRCore] ABAR phys=");
    serial_puthex(abar);
    serial_puts(" virt=");
    serial_puthex((uint64_t)(uintptr_t)g_hba);
    serial_puts("\n");

    uint32_t vs  = hba_rd(HBA_VS);
    uint32_t cap = hba_rd(HBA_CAP);
    if (vs == 0xFFFFFFFFu || cap == 0xFFFFFFFFu) {
        serial_puts("[SRCore] no controller at ABAR\n");
        g_hba = 0;
        return false;
    }

    /* 打开 AHCI 模式，全局关中断（纯轮询） */
    hba_wr(HBA_GHC, hba_rd(HBA_GHC) | HBA_GHC_AE);
    hba_wr(HBA_GHC, hba_rd(HBA_GHC) & ~HBA_GHC_IE);

    uint32_t pi = hba_rd(HBA_PI);
    uint32_t nports = (cap & 0x1Fu) + 1;
    if (nports > 32) nports = 32;

    serial_puts("[SRCore] version=");
    serial_puthex(vs);
    serial_puts(" cap=");
    serial_puthex(cap);
    serial_puts(" ports_impl=");
    serial_puthex(pi);
    serial_puts("\n");

    for (uint32_t p = 0; p < nports && g_disk_count < AHCI_MAX_DISKS; p++) {
        if (!(pi & (1u << p))) continue;
        uint32_t ssts = p_rd((int)p, PORTP_SSTS);
        uint32_t sig  = p_rd((int)p, PORTP_SIG);
        if ((ssts & SSTS_DET_MASK) != SSTS_DET_PRESENT) continue;
        if (sig != SATA_SIG_ATA) continue;         /* 只接 SATA 硬盘 */

        int idx = g_disk_count;
        serial_puts("[SRCore] disk found on port ");
        serial_putdec(p);
        serial_puts(" -> disk");
        serial_putdec((uint64_t)idx);
        serial_puts(" ssts=");
        serial_puthex(ssts);
        serial_puts(" sig=");
        serial_puthex(sig);
        serial_puts("\n");

        g_ports[idx] = (int)p;
        g_disk_count = idx + 1;     /* 让 ahci_run_cmd 能访问这个临时槽位 */

        port_stop((int)p);
        if (!port_start((int)p, idx)) {
            serial_puts("[SRCore] port start failed, skipping disk\n");
            g_disk_count = idx;
            g_ports[idx] = -1;
            continue;
        }

        /* IDENTIFY DEVICE：取总扇区数（优先 LBA48） */
        memset(g_ident, 0, sizeof(g_ident));
        if (!ahci_run_cmd(idx, ATA_CMD_IDENTIFY, 0, 0, g_ident, 512,
                          false, AHCI_POLL_MAX)) {
            serial_puts("[SRCore] identify failed, skipping disk\n");
            port_stop((int)p);
            g_disk_count = idx;
            g_ports[idx] = -1;
            continue;
        }

        uint64_t sectors = (uint64_t)g_ident[100]
                         | ((uint64_t)g_ident[101] << 16)
                         | ((uint64_t)g_ident[102] << 32)
                         | ((uint64_t)g_ident[103] << 48);
        if (sectors == 0)
            sectors = (uint64_t)g_ident[60] | ((uint64_t)g_ident[61] << 16);
        if (sectors == 0) {
            serial_puts("[SRCore] zero-size disk, skipping\n");
            port_stop((int)p);
            g_disk_count = idx;
            g_ports[idx] = -1;
            continue;
        }

        g_disk_sectors[idx] = sectors;
        serial_puts("[SRCore] disk");
        serial_putdec((uint64_t)idx);
        serial_puts(" ok, sectors=");
        serial_putdec(sectors);
        serial_puts("\n");
    }

    if (g_disk_count == 0) {
        serial_puts("[SRCore] no SATA disk attached\n");
        return false;
    }

    serial_puts("[SRCore] PRDBC backfill=");
    serial_puts(g_prdbc_ok == 1 ? "yes (DMA byte count strictly verified)"
                                : "no (rely on PxCI retire + TFD/SERR)");
    serial_puts("\n");
    serial_puts("[SRCore] total disks=");
    serial_putdec((uint64_t)g_disk_count);
    serial_puts("\n");
    g_ready = true;
    return true;
}

bool ahci_present(void)     { return g_ready && g_disk_count > 0; }
int ahci_disk_count(void)   { return g_disk_count; }
uint64_t ahci_disk_sectors(int disk_idx) {
    if (disk_idx < 0 || disk_idx >= g_disk_count) return 0;
    return g_disk_sectors[disk_idx];
}

/* 兼容旧代码：单盘入口默认访问 disk0。新代码请使用带 disk_idx 的接口。 */
uint64_t ahci_sectors(void) { return ahci_disk_sectors(0); }

/* 多扇区一次命令搬运的核心：把 count 个扇区切成 ≤ AHCI_BOUNCE_SECTORS
   的分段，每段发一条 READ/WRITE DMA EXT：
     - fis->countl/counth = 段内扇区数 n（FIS 的 count 字段支持多扇区）；
     - PRDT[0].dbc = n*512 - 1（一次 DMA 连续搬运 n*512 字节）；
     - PRDT 永远指向 4KB 对齐的静态 g_bounce，调用方缓冲区只做 memcpy。
   任一段失败即整体失败并返回 false。 */
static bool ahci_rw_sectors(int disk_idx, uint8_t ata_cmd, uint64_t lba,
                            uint32_t count, void *buf, bool write,
                            uint32_t poll_max) {
    if (!g_ready || count == 0) return false;
    uint64_t sectors = ahci_disk_sectors(disk_idx);
    if (lba >= sectors || (uint64_t)count > sectors - lba) return false;

    uint8_t *p = (uint8_t *)buf;
    while (count) {
        uint32_t n = (count > AHCI_BOUNCE_SECTORS)
                         ? AHCI_BOUNCE_SECTORS
                         : count;
        uint32_t bytes = n * 512u;

        if (write) memcpy(g_bounce, p, bytes);   /* 写：先灌入对齐缓冲 */

        if (!ahci_run_cmd(disk_idx, ata_cmd, lba, (uint16_t)n, g_bounce, bytes,
                          write, poll_max))
            return false;

        if (!write) memcpy(p, g_bounce, bytes);  /* 读：从对齐缓冲取回 */

        lba   += n;
        p     += bytes;
        count -= n;
    }
    return true;
}

bool ahci_read_sectors(int disk_idx, uint64_t lba, uint32_t count, void *buf) {
    bool ok = ahci_rw_sectors(disk_idx, ATA_CMD_READ_DMA_EX, lba, count, buf,
                              false, AHCI_POLL_MAX);
    if (ok) read_sectors_total += count;
    return ok;
}

bool ahci_write_sectors(int disk_idx, uint64_t lba, uint32_t count,
                        const void *buf) {
    bool ok = ahci_rw_sectors(disk_idx, ATA_CMD_WRITE_DMA_EX, lba, count,
                              (void *)buf, true, AHCI_WRITE_POLL_MAX);
    if (ok) write_sectors_total += count;
    return ok;
}

bool ahci_read_sector(uint64_t lba, void *buf) {
    return ahci_read_sectors(0, lba, 1, buf);
}

/* 写一个扇区（DMA，Host->Device）。写路径的正确性由 ahci_run_cmd /
   ahci_exec_once 保证，要点全在那两个函数里，这里是清单：
   1) Command Header 的 W 位（flags bit6）置 1，并在下发前“从内存读回”严格
      校验；读命令则必须为 0；
   2) Register H2D FIS 的 C 位（byte1 bit7，pmport 字段）置 1，同样读回校验。
      C 位在 pmport 字段而不在 command 字段 —— 把操作码写成 0x35|0x80=0xB5
      是非法 ATA 命令，设备会直接 ABRT；
   3) 下发前 sfence，保证命令表(WB 内存)的写入先于 PxCI(UC MMIO) 生效，
      写完 PxCI 再读回，强制 posted write 落地；
   4) 只用 PxCI 清零判定“命令退休”，退休之后才检查 TFD 的 ERR/DF 与 SERR
      （ERR/DF 是粘滞位，拿它做提前退出会把遗留错误当成本次结果）；
   5) 退休后再校验 HBA 回填的 PRDBC == 本次命令的字节数（count*512），
      确认数据 DMA 真的搬完了；
   6) 任一步失败都会做端口恢复并重试（上限 AHCI_CMD_RETRY），重试用尽只返回
      false。所有等待都有轮询上限，任何分支都不会挂死。
   DMA 地址：buf 经 virt_to_phys() 换算为真实物理地址
   （.bss 缓冲走内核镜像换算，其余走 HHDM），见文件头说明。

   落盘策略：本函数不逐扇区 FLUSH —— 每来一次 FLUSH CACHE EXT 宿主机就要
   fsync 一遍，逐扇区 FLUSH 会把启动拖到分钟级并诱发超时。
   ATA_CMD_FLUSH_EXT 由 fs.c 的 fs_sync() 在每一批脏块写完后统一下发，
   而 fs_write_file/fs_write/fs_create/mkfs 的收尾都会调用 fs_sync()，
   因此任何一次写 API 返回时，数据都已 FLUSH 落盘。 */
bool ahci_write_sector(uint64_t lba, const void *buf) {
    return ahci_write_sectors(0, lba, 1, buf);
}

/* 刷写指定设备的易失性写缓存（FLUSH CACHE EXT，0xEA）。
   调用前提：之前所有写命令都已完成（PxCI 已清零，TFD 无 BSY/DRQ）。 */
bool ahci_flush_disk(int disk_idx) {
    if (!g_ready || disk_idx < 0 || disk_idx >= g_disk_count) return false;
    return ahci_run_cmd(disk_idx, ATA_CMD_FLUSH_EXT, 0, 0, 0, 0,
                        false, AHCI_FLUSH_POLL_MAX);
}

/* 兼容旧代码：默认刷 disk0。 */
bool ahci_flush(void) { return ahci_flush_disk(0); }
