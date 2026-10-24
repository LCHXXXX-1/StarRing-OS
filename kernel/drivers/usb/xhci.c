#include "xhci.h"
#include "arch/x86_64/cpu.h"
#include "drivers/pci.h"
#include "drivers/serial.h"
#include "drivers/keyboard.h"
#include "drivers/input.h"
#include "drivers/timer.h"
#include "lib/mem.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

/* ============================================================
 *  0. 常量与编译期检查
 * ============================================================ */

#define XHCI_MAX_SLOTS       16u      /* 本驱动静态上下文能容纳的槽位上限 */
#define XHCI_CMD_TRBS        256u     /* 命令环 TRB 数（含末尾 Link TRB） */
#define XHCI_EVT_TRBS        256u     /* 事件环 TRB 数（硬件自动回绕） */
#define XHCI_EP_TRBS         64u      /* 每个传输环 TRB 数（含 Link TRB） */
#define XHCI_MAX_SCRATCHPAD  64u      /* 最多支持的 scratchpad 页数 */

/* 所有等待都是“有上限的自旋”，绝对不允许死循环。数值按 QEMU 和
   普通真实硬件的量级给足余量；失败就打印并退出，不影响 PS/2。 */
#define XHCI_TIMEOUT_CMD     4000000u
#define XHCI_TIMEOUT_XFER    4000000u
#define XHCI_TIMEOUT_RESET   4000000u
#define XHCI_TIMEOUT_PORT    4000000u

/* 调试开关：正式运行保持 0。置 1 后会打印每次 HID 报告，便于验证。 */
#define XHCI_DEBUG           0

/* ---- TRB 类型（xHCI 规范 Table 6-91） ---- */
#define TRB_NORMAL           1u
#define TRB_SETUP            2u
#define TRB_DATA             3u
#define TRB_STATUS           4u
#define TRB_LINK             6u
#define TRB_ENABLE_SLOT      9u
#define TRB_DISABLE_SLOT     10u
#define TRB_ADDR_DEV         11u
#define TRB_CONFIG_EP        12u
#define TRB_EVAL_CONTEXT     13u
#define TRB_TRANSFER         32u
#define TRB_COMPLETION       33u
#define TRB_PORT_STATUS      34u

/* ---- TRB control 位 ---- */
#define TRB_CYCLE            (1u << 0)
#define TRB_ISP              (1u << 2)
#define TRB_CHAIN            (1u << 4)
#define TRB_IOC              (1u << 5)
#define TRB_IDT              (1u << 6)
#define TRB_DIR_IN           (1u << 16)
#define TRB_TYPE(t)          ((uint32_t)(t) << 10)
#define TRB_LINK_TC          (1u << 1)

/* ---- 完成码 ---- */
#define COMP_SUCCESS         1u
#define COMP_STALL           6u
#define COMP_SHORT_PACKET    13u

/* ---- 操作寄存器偏移 ---- */
#define OP_USBCMD            0x00u
#define OP_USBSTS            0x04u
#define OP_PAGESIZE          0x08u
#define OP_CRCR              0x18u
#define OP_DCBAAP            0x30u
#define OP_CONFIG            0x38u
#define OP_PORTSC_BASE       0x400u
#define PORTSC_SIZE          0x10u

#define USBCMD_RUN           (1u << 0)
#define USBCMD_HCRST         (1u << 1)
#define USBSTS_HCHALTED      (1u << 0)
#define USBSTS_CNR           (1u << 11)

/* ---- PORTSC 位 ---- */
#define PORTSC_CCS           (1u << 0)
#define PORTSC_PED           (1u << 1)
#define PORTSC_PR            (1u << 4)
#define PORTSC_PLS_MASK      (0xFu << 5)
#define PORTSC_PP            (1u << 9)
#define PORTSC_SPEED_MASK    (0xFu << 10)
#define PORTSC_PIC_MASK      (0x3u << 14)
#define PORTSC_PRC           (1u << 21)
/* RW1C 位：CSC/PEC/WRC/OCC/PRC/PLC/CEC */
#define PORTSC_RWC           0x00FE0000u
/* 写 PORTSC 时必须保留的 RWS 位。绝不能把 PED=1 写回去：在真实
   控制器上 PED 是 RW1CS，写 1 会禁用刚刚复位成功的端口。 */
#define PORTSC_KEEP          (PORTSC_PP | PORTSC_PIC_MASK | \
                              (1u << 25) | (1u << 26) | (1u << 27))

/* ---- 输入上下文 Add/Drop flags ---- */
#define CTX_SLOT             (1u << 0)
#define CTX_EP0              (1u << 1)

/* ---- Slot/EP 上下文字段 ---- */
#define SLOT_SPEED(s)        (((uint32_t)(s) & 0xFu) << 20)
#define SLOT_ENTRIES(n)      (((uint32_t)(n) & 0x1Fu) << 27)
#define SLOT_ROOT_PORT(p)    (((uint32_t)(p) & 0xFFu) << 16)

#define EP_TYPE_CONTROL      4u
#define EP_TYPE_INT_IN       7u
#define EP_CERR_3            (3u << 1)
#define EP_TYPE(t)           (((uint32_t)(t) & 7u) << 3)
#define EP_MAXPACKET(m)      (((uint32_t)(m) & 0xFFFFu) << 16)
#define EP_INTERVAL(i)       (((uint32_t)(i) & 0xFFu) << 16)

/* ---- PCI ---- */
#define XHCI_PCI_CLASS       0x0Cu
#define XHCI_PCI_SUBCLASS    0x03u
#define XHCI_PCI_PROGIF      0x30u
#define PCI_CMD_INTX_DISABLE (1u << 10)

/* ---- MMIO 虚拟地址：避开 AHCI 的 0xFFFFFFFFC0000000。 ---- */
#define XHCI_MMIO_VA         0xFFFFFFFFC0400000ULL
#define PTE_P                0x001ULL
#define PTE_RW               0x002ULL
#define PTE_PWT              0x008ULL
#define PTE_PCD              0x010ULL
#define PTE_PS               0x080ULL

/* ============================================================
 *  1. 数据结构
 * ============================================================ */

/* TRB 是 16 字节、16 字节对齐的硬件描述符。 parameter 是 64 位字段，
   status/control 各 32 位；用 packed 保证编译器不插入填充。 */
struct xhci_trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} __attribute__((packed));

_Static_assert(sizeof(struct xhci_trb) == 16, "xHCI TRB must be 16 bytes");

/* Slot Context（32 字节）：设备速度、根端口、上下文数量等。 */
struct xhci_slot_ctx {
    uint32_t dw0;
    uint32_t dw1;
    uint32_t dw2;
    uint32_t dw3;
    uint32_t rsv[4];       /* 规范要求 Slot Context 占 32 字节 */
} __attribute__((packed));

/* Endpoint Context（32 字节）：端点类型、最大包长、TR 出队指针。 */
struct xhci_ep_ctx {
    uint32_t dw0;
    uint32_t dw1;
    uint64_t deq;
    uint32_t dw4;
    uint32_t rsv[3];
} __attribute__((packed));

/* Device Context（1024 字节 = Slot + 31 个 Endpoint Context）。
   末尾留 1KB 余量，某些控制器/固件对上下文抓取范围比规范更宽。 */
struct xhci_dev_ctx {
    struct xhci_slot_ctx slot;
    struct xhci_ep_ctx   ep[31];
    uint8_t              pad[1024];
} __attribute__((packed));

/* Input Context：多一个 32 字节的 Input Control Context。 */
struct xhci_input_ctx {
    uint32_t             ctrl[8];     /* drop[0], add[1], 其余保留 */
    struct xhci_slot_ctx slot;
    struct xhci_ep_ctx   ep[31];
} __attribute__((packed));

_Static_assert(sizeof(struct xhci_slot_ctx) == 32, "slot context size");
_Static_assert(sizeof(struct xhci_ep_ctx) == 32, "endpoint context size");

/* Event Ring Segment Table entry（16 字节，64 字节对齐）。 */
struct xhci_erst_entry {
    uint64_t seg_addr;
    uint32_t seg_size;
    uint32_t rsv;
} __attribute__((packed));

_Static_assert(sizeof(struct xhci_erst_entry) == 16, "ERST entry size");

/* 事件环里我们关心的字段。 */
struct xhci_event {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
    uint32_t residual;
    uint8_t  type;
    uint8_t  code;
    uint8_t  slot;
    uint8_t  ep;
};

/* 软件生产者环状态。Link TRB 固定在 size-1。 */
struct xhci_ring {
    struct xhci_trb *trbs;
    uint64_t         phys;
    uint16_t         size;
    uint16_t         enq;
    uint8_t          cycle;
};

/* ============================================================
 *  2. 静态 DMA 内存（禁止 malloc）
 * ============================================================ */

static uint64_t g_dcbaa[256] __attribute__((aligned(64)));
static struct xhci_trb g_cmd_trbs[XHCI_CMD_TRBS] __attribute__((aligned(4096)));
static struct xhci_trb g_evt_trbs[XHCI_EVT_TRBS] __attribute__((aligned(4096)));
static struct xhci_trb g_ep0_trbs[XHCI_MAX_SLOTS][XHCI_EP_TRBS]
    __attribute__((aligned(4096)));
/* 键盘与鼠标各一条 Interrupt IN 传输环、各一个报告缓冲，互不覆盖。 */
static struct xhci_trb g_kbd_trbs[XHCI_EP_TRBS] __attribute__((aligned(4096)));
static struct xhci_trb g_mouse_trbs[XHCI_EP_TRBS] __attribute__((aligned(4096)));
static struct xhci_erst_entry g_erst[1] __attribute__((aligned(64)));
static struct xhci_dev_ctx g_dev_ctx[XHCI_MAX_SLOTS]
    __attribute__((aligned(4096)));
static struct xhci_input_ctx g_in_ctx __attribute__((aligned(4096)));
static uint8_t g_ctrl_buf[512] __attribute__((aligned(64)));
static uint8_t g_kbd_buf[64] __attribute__((aligned(64)));
static uint8_t g_mouse_buf[64] __attribute__((aligned(64)));
static uint64_t g_scratch_ptrs[XHCI_MAX_SCRATCHPAD] __attribute__((aligned(64)));

/* MMIO 2MB 页表（放在 .bss，不依赖 malloc）。 */
static uint64_t g_mmio_pdpt[512] __attribute__((aligned(4096)));
static uint64_t g_mmio_pd[512]   __attribute__((aligned(4096)));

/* ============================================================
 *  3. 运行时状态
 * ============================================================ */

static volatile uint8_t *g_mmio = 0;
static uint32_t g_op = 0;          /* Operational Base  */
static uint32_t g_db = 0;          /* Doorbell Array    */
static uint32_t g_rt = 0;          /* Runtime Register  */
static uint32_t g_max_slots = 0;
static uint32_t g_max_ports = 0;
static uint16_t g_hci_version = 0;
static bool     g_inited = false;
static bool     g_ready = false;

static struct xhci_ring g_cmd_ring;
static struct xhci_ring g_ep0_ring[XHCI_MAX_SLOTS];

static uint64_t g_evt_phys = 0;
static uint16_t g_evt_index = 0;
static uint8_t  g_evt_cycle = 1;   /* 事件环消费者周期状态，初始为 1 */

static uint16_t g_ep0_max[XHCI_MAX_SLOTS + 1];

/* ---- HID 设备状态（每类设备一个槽位） ----
   Boot Protocol：键盘报告 8 字节、鼠标报告 3 字节。两类设备各有
   独立的 slot / DCI / 传输环 / 报告缓冲，事件按 (slot, dci) 分发。 */
#define HID_DEV_KBD     0
#define HID_DEV_MOUSE   1
#define HID_DEV_COUNT   2
#define HID_REPORT_MAX  8u

struct hid_dev {
    bool             ready;
    bool             armed;
    uint8_t          proto;       /* 1 = keyboard, 2 = mouse */
    uint8_t          slot;
    uint8_t          dci;
    uint8_t          iface;
    uint8_t          ep_addr;
    uint8_t          port;        /* 所在根端口（热插拔扫描时跳过） */
    uint8_t          prev[6];     /* 键盘上一次按下的 6 个 usage */
    struct xhci_ring ring;
    struct xhci_trb *trbs;
    uint8_t         *buf;
};

static struct hid_dev g_hid[HID_DEV_COUNT] = {
    [HID_DEV_KBD]   = { .trbs = g_kbd_trbs,   .buf = g_kbd_buf   },
    [HID_DEV_MOUSE] = { .trbs = g_mouse_trbs, .buf = g_mouse_buf },
};

static bool     g_kbd_caps = false;     /* Caps Lock 状态 */
static uint64_t g_last_poll_tick = 0;
static uint64_t g_last_scan_tick = 0;
static uint32_t g_port_failed = 0;      /* 枚举失败过的端口位图；拔掉后清除 */

static inline void cpu_pause(void) {
    __asm__ volatile ("pause");
}
static inline void mem_barrier(void) {
    __asm__ volatile ("sfence" ::: "memory");
}
static inline void load_barrier(void) {
    __asm__ volatile ("lfence" ::: "memory");
}

/* ============================================================
 *  4. 物理地址、MMIO 访问
 * ============================================================ */

/* 静态数组的物理地址：优先通过当前页表翻译，避免再声明一份 Limine
   kernel-address request（同一个 request ID 只能出现一次）。所有 DMA
   对象都保证不超过一个 4KB 页（见上面的对齐）。 */
static uint64_t xhci_phys(const void *p) {
    uint64_t phys = 0;
    if (vmm_ready() && vmm_translate((uint64_t)(uintptr_t)p, &phys, 0))
        return phys;
    return (uint64_t)(uintptr_t)p;
}

static inline uint32_t mmio_rd32(uint32_t off) {
    return *(volatile uint32_t *)(g_mmio + off);
}
static inline void mmio_wr32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_mmio + off) = v;
}
static inline void mmio_wr64(uint32_t off, uint64_t v) {
    /* xHCI 的 64 位寄存器必须“先低后高”：CRCR/ERSTBA 的高半部写入
       会触发控制器用当前低半部重新初始化命令环/事件环。若先写高，
       控制器会看到旧的低半部（通常是 0），把环指针设成 0 并报 HCE。 */
    mmio_wr32(off, (uint32_t)v);
    mmio_wr32(off + 4, (uint32_t)(v >> 32));
}

/* 建立 2MB 非缓存映射。与 ahci.c 的 map_mbar 思路一致，但使用独立
   虚拟地址区间，避免与 AHCI 的 0xFFFFFFFFC0000000 冲突。 */
static void xhci_map_2mb(uint64_t va, uint64_t pa) {
    if (!vmm_ready() || !vmm_hhdm()) return;

    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    uint64_t hhdm = vmm_hhdm();

    uint64_t *pml4 = (uint64_t *)(uintptr_t)(hhdm + (cr3 & ~0xFFFull));
    uint64_t i4 = (va >> 39) & 0x1FF;
    if (!(pml4[i4] & PTE_P)) {
        memset(g_mmio_pdpt, 0, sizeof(g_mmio_pdpt));
        pml4[i4] = xhci_phys(g_mmio_pdpt) | PTE_P | PTE_RW;
    }

    uint64_t *pdpt = (uint64_t *)(uintptr_t)
        (hhdm + (pml4[i4] & ~0xFFFull));
    uint64_t i3 = (va >> 30) & 0x1FF;
    if (!(pdpt[i3] & PTE_P)) {
        memset(g_mmio_pd, 0, sizeof(g_mmio_pd));
        pdpt[i3] = xhci_phys(g_mmio_pd) | PTE_P | PTE_RW;
    }

    uint64_t *pd = (uint64_t *)(uintptr_t)
        (hhdm + (pdpt[i3] & ~0xFFFull));
    uint64_t i2 = (va >> 21) & 0x1FF;
    pd[i2] = (pa & ~0x1FFFFFULL) | PTE_P | PTE_RW | PTE_PWT | PTE_PCD | PTE_PS;

    __asm__ volatile ("invlpg (%0)" :: "r"((void *)(uintptr_t)va) : "memory");
}

static volatile uint8_t *xhci_map_mmio(uint64_t bar) {
    uint64_t base = bar & ~0x1FFFFFULL;

    /* 没有分页信息时退化为直接访问物理地址（和 ahci.c 一致）。 */
    if (!vmm_ready() || !vmm_hhdm())
        return (volatile uint8_t *)(uintptr_t)bar;

    /* 映射 4MB，覆盖 BAR 可能跨越 2MB 边界的情况。 */
    xhci_map_2mb(XHCI_MMIO_VA, base);
    xhci_map_2mb(XHCI_MMIO_VA + 0x200000ULL, base + 0x200000ULL);
    return (volatile uint8_t *)(uintptr_t)
        (XHCI_MMIO_VA + (bar - base));
}

/* ============================================================
 *  5. PCI 扫描
 * ============================================================ */

static bool xhci_pci_find(uint64_t *out_bar) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t hdr = pci_read_config_dword((uint8_t)bus, slot, 0, 0x0C);
            uint8_t nfunc = (hdr & 0x00800000u) ? 8 : 1;

            for (uint8_t func = 0; func < nfunc; func++) {
                uint32_t id = pci_read_config_dword((uint8_t)bus, slot, func,
                                                     PCI_CFG_VENDOR_ID);
                uint16_t vendor = (uint16_t)(id & 0xFFFFu);
                uint16_t device = (uint16_t)(id >> 16);
                if (vendor == 0xFFFFu) continue;

                uint32_t cls = pci_read_config_dword((uint8_t)bus, slot, func,
                                                     PCI_CFG_REVISION);
                uint8_t prog_if  = (uint8_t)((cls >> 8)  & 0xFFu);
                uint8_t subclass = (uint8_t)((cls >> 16) & 0xFFu);
                uint8_t class    = (uint8_t)((cls >> 24) & 0xFFu);

                if (class != XHCI_PCI_CLASS ||
                    subclass != XHCI_PCI_SUBCLASS ||
                    prog_if != XHCI_PCI_PROGIF)
                    continue;

                uint32_t bar0 = pci_read_config_dword((uint8_t)bus, slot, func,
                                                      PCI_CFG_BAR0);
                if (bar0 & 0x1u) continue;             /* I/O BAR，不是 MMIO */

                uint64_t bar = (uint64_t)(bar0 & ~0xFu);
                if ((bar0 & 0x6u) == 0x4u) {           /* 64-bit BAR */
                    uint32_t bar1 = pci_read_config_dword((uint8_t)bus, slot,
                                                          func,
                                                          PCI_CFG_BAR0 + 4);
                    bar |= ((uint64_t)bar1 << 32);
                }
                if (!bar) continue;

                /* 打开内存译码 + Bus Master，并关掉传统 INTx。
                   没有 Bus Master 时 xHCI 无法读 DCBAA/TRB，命令永远
                   不会完成；这与 AHCI 的经典“空盘写超时”是同一原因。 */
                uint16_t cmd = pci_read_config_word((uint8_t)bus, slot, func,
                                                    PCI_CFG_COMMAND);
                uint16_t want = (uint16_t)(cmd | PCI_CMD_MEM_SPACE |
                                           PCI_CMD_BUS_MASTER |
                                           PCI_CMD_INTX_DISABLE);
                if (want != cmd) {
                    pci_write_config_word((uint8_t)bus, slot, func,
                                          PCI_CFG_COMMAND, want);
                    cmd = pci_read_config_word((uint8_t)bus, slot, func,
                                               PCI_CFG_COMMAND);
                }

                serial_puts("[SRCore] xHCI controller @ bus=");
                serial_putdec(bus);
                serial_puts(" slot=");
                serial_putdec(slot);
                serial_puts(" func=");
                serial_putdec(func);
                serial_puts(" vendor=");
                serial_puthex(vendor);
                serial_puts(" device=");
                serial_puthex(device);
                serial_puts(" BAR=");
                serial_puthex(bar);
                serial_puts(" cmd=");
                serial_puthex(cmd);
                serial_puts("\n");

                if (!(cmd & PCI_CMD_MEM_SPACE) ||
                    !(cmd & PCI_CMD_BUS_MASTER)) {
                    serial_puts("[SRCore] xHCI: memory/bus-master enable failed\n");
                    return false;
                }

                *out_bar = bar;
                return true;
            }
        }
    }

    /* 没有 xHCI 控制器：静默返回，保留 PS/2 键盘。 */
    return false;
}

/* ============================================================
 *  6. BIOS/OS 握手、复位、寄存器初始化
 * ============================================================ */

/* 遍历 Extended Capabilities，找 USB Legacy Support（ID 1）。
   如果 BIOS 仍持有所有权，按 xHCI 规范设置 OS Owned 并等待其释放。
   这一步对部分真实主板的 xHCI 端口可用性很重要；QEMU 通常没有这个
   扩展能力，函数会直接返回。 */
static void xhci_legacy_handoff(uint32_t hccparams1) {
    uint32_t off = ((hccparams1 >> 16) & 0xFFFFu) << 2;
    if (!off) return;

    for (int guard = 0; guard < 64 && off; guard++) {
        if (off + 12 >= 0x400000u) return;
        uint32_t d0 = mmio_rd32(off);
        uint8_t id = (uint8_t)(d0 & 0xFFu);
        uint8_t next = (uint8_t)((d0 >> 8) & 0xFFu);

        if (id == 1) {                        /* USB Legacy Support */
            uint32_t leg = mmio_rd32(off + 4);
            if (leg & (1u << 16)) {           /* BIOS Owned */
                mmio_wr32(off + 4, leg | (1u << 24)); /* OS Owned */
                for (uint32_t i = 0; i < 1000000u; i++) {
                    leg = mmio_rd32(off + 4);
                    if (!(leg & (1u << 16))) break;
                    cpu_pause();
                }
                serial_puts("[SRCore] xHCI legacy handoff ");
                serial_puts((leg & (1u << 16)) ? "timeout\n" : "ok\n");
            }
            return;
        }

        if (!next) return;
        off += (uint32_t)next << 2;
    }
}

/* Scratchpad Buffer Array：HCSPARAMS2 报告需要 scratchpad 时，DCBAA[0]
   必须指向一个 64 位物理地址数组，每一项指向一个 4KB scratchpad 页。
   这里用 PMM 分配页帧、静态数组存指针，仍然没有 malloc。 */
static bool xhci_setup_scratchpad(uint32_t hcs2) {
    uint32_t count = ((hcs2 >> 21) & 0x1Fu) << 5;
    count |= (hcs2 & 0x1Fu);

    if (count == 0) {
        g_dcbaa[0] = 0;
        return true;
    }
    if (count > XHCI_MAX_SCRATCHPAD) {
        serial_puts("[SRCore] xHCI: too many scratchpad buffers: ");
        serial_putdec(count);
        serial_puts("\n");
        return false;
    }
    if (!pmm_ready()) {
        serial_puts("[SRCore] xHCI: scratchpad requires PMM\n");
        return false;
    }

    uint64_t hhdm = vmm_hhdm();
    for (uint32_t i = 0; i < count; i++) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame) {
            serial_puts("[SRCore] xHCI: scratchpad alloc failed\n");
            for (uint32_t j = 0; j < i; j++) pmm_free_frame(g_scratch_ptrs[j]);
            return false;
        }
        g_scratch_ptrs[i] = frame;
        if (hhdm)
            memset((void *)(uintptr_t)(frame + hhdm), 0, PMM_PAGE_SIZE);
        else
            memset((void *)(uintptr_t)frame, 0, PMM_PAGE_SIZE);
    }

    g_dcbaa[0] = xhci_phys(g_scratch_ptrs);
    serial_puts("[SRCore] xHCI scratchpad buffers: ");
    serial_putdec(count);
    serial_puts("\n");
    return true;
}

static bool xhci_wait_cnr_clear(void) {
    for (uint32_t i = 0; i < XHCI_TIMEOUT_RESET; i++) {
        if (!(mmio_rd32(g_op + OP_USBSTS) & USBSTS_CNR)) return true;
        cpu_pause();
    }
    return false;
}

bool xhci_init(void) {
    if (g_inited) return g_ready;
    g_inited = true;

    uint64_t bar = 0;
    if (!xhci_pci_find(&bar)) return false;

    g_mmio = xhci_map_mmio(bar);

    uint32_t cap = mmio_rd32(0x00);
    g_op = cap & 0xFFu;
    g_hci_version = (uint16_t)(cap >> 16);

    uint32_t hcs1 = mmio_rd32(0x04);
    uint32_t hcs2 = mmio_rd32(0x08);
    uint32_t hcc  = mmio_rd32(0x10);

    g_max_ports = (hcs1 >> 24) & 0xFFu;
    g_max_slots = hcs1 & 0xFFu;
    if (g_max_slots > XHCI_MAX_SLOTS) g_max_slots = XHCI_MAX_SLOTS;

    /* CSZ=1 表示 64 字节上下文，本驱动只实现最常见的 32 字节上下文。 */
    if (hcc & (1u << 2)) {
        serial_puts("[SRCore] xHCI: 64-byte contexts unsupported\n");
        return false;
    }
    if (!g_max_slots || !g_max_ports || !g_op) {
        serial_puts("[SRCore] xHCI: invalid HCSPARAMS1\n");
        return false;
    }

    /* Doorbell / Runtime 偏移在独立寄存器 0x14 / 0x18 中。 */
    g_db = mmio_rd32(0x14) & ~0x3u;
    g_rt = mmio_rd32(0x18) & ~0x1Fu;

    if (!g_db || !g_rt ||
        g_db >= 0x400000u || g_rt >= 0x400000u ||
        g_op + OP_PORTSC_BASE + g_max_ports * PORTSC_SIZE > 0x400000u) {
        serial_puts("[SRCore] xHCI: register offset out of mapped range\n");
        return false;
    }

    serial_puts("[SRCore] xHCI version=");
    serial_puthex(g_hci_version);
    serial_puts(" slots=");
    serial_putdec(g_max_slots);
    serial_puts(" ports=");
    serial_putdec(g_max_ports);
    serial_puts(" op=");
    serial_puthex(g_op);
    serial_puts(" db=");
    serial_puthex(g_db);
    serial_puts(" rt=");
    serial_puthex(g_rt);
    serial_puts(" hcs2=");
    serial_puthex(hcs2);
    serial_puts(" hcc=");
    serial_puthex(hcc);
    serial_puts("\n");

    xhci_legacy_handoff(hcc);

    if (!xhci_wait_cnr_clear()) {
        serial_puts("[SRCore] xHCI: controller not ready (CNR)\n");
        return false;
    }

    /* HCRST：写 1 后等硬件自动清零；然后再等 CNR 清零。 */
    mmio_wr32(g_op + OP_USBCMD, USBCMD_HCRST);
    uint32_t i;
    for (i = 0; i < XHCI_TIMEOUT_RESET; i++) {
        if (!(mmio_rd32(g_op + OP_USBCMD) & USBCMD_HCRST)) break;
        cpu_pause();
    }
    if (i == XHCI_TIMEOUT_RESET) {
        serial_puts("[SRCore] xHCI: reset timeout\n");
        return false;
    }
    if (!xhci_wait_cnr_clear()) {
        serial_puts("[SRCore] xHCI: CNR after reset timeout\n");
        return false;
    }

    /* 清空所有 DMA 结构。设备上下文必须 64 字节对齐；静态数组已保证。 */
    memset(g_dcbaa, 0, sizeof(g_dcbaa));
    memset(g_cmd_trbs, 0, sizeof(g_cmd_trbs));
    memset(g_evt_trbs, 0, sizeof(g_evt_trbs));
    memset(g_ep0_trbs, 0, sizeof(g_ep0_trbs));
    memset(g_kbd_trbs, 0, sizeof(g_kbd_trbs));
    memset(g_mouse_trbs, 0, sizeof(g_mouse_trbs));
    memset(g_dev_ctx, 0, sizeof(g_dev_ctx));
    memset(&g_in_ctx, 0, sizeof(g_in_ctx));
    memset(g_ep0_max, 0, sizeof(g_ep0_max));

    if (!xhci_setup_scratchpad(hcs2)) return false;

    /* Command Ring：256 TRB，最后一个是 Link TRB。 */
    g_cmd_ring.trbs = g_cmd_trbs;
    g_cmd_ring.phys = xhci_phys(g_cmd_trbs);
    g_cmd_ring.size = XHCI_CMD_TRBS;
    g_cmd_ring.enq = 0;
    g_cmd_ring.cycle = 1;
    g_cmd_trbs[XHCI_CMD_TRBS - 1].parameter = g_cmd_ring.phys;
    g_cmd_trbs[XHCI_CMD_TRBS - 1].status = 0;
    g_cmd_trbs[XHCI_CMD_TRBS - 1].control =
        TRB_TYPE(TRB_LINK) | TRB_LINK_TC | TRB_CYCLE;

    /* Event Ring：1 个 segment，256 TRB。 */
    g_evt_phys = xhci_phys(g_evt_trbs);
    g_evt_index = 0;
    g_evt_cycle = 1;
    g_erst[0].seg_addr = g_evt_phys;
    g_erst[0].seg_size = XHCI_EVT_TRBS;
    g_erst[0].rsv = 0;

    /* DCBAAP / CRCR / CONFIG。CRCR 的 bit0 是 RCS，必须为 1。 */
    mem_barrier();                    /* 确保 DCBAA/scratchpad/ERST 已落内存 */
    mmio_wr64(g_op + OP_DCBAAP, xhci_phys(g_dcbaa));
    mmio_wr64(g_op + OP_CRCR, g_cmd_ring.phys | 1u);
    mmio_wr32(g_op + OP_CONFIG, g_max_slots);

    /* Interrupter 0：不打开中断，只配置事件环。 */
    uint32_t ir = g_rt + 0x20u;
    mmio_wr32(ir + 0x00, 0);                        /* IMAN: IE=0 */
    mmio_wr32(ir + 0x04, 0);                        /* IMOD */
    mmio_wr32(ir + 0x08, 1);                        /* ERSTSZ = 1 segment */
    mmio_wr64(ir + 0x10, xhci_phys(g_erst));        /* ERSTBA */
    mmio_wr64(ir + 0x18, g_evt_phys);               /* ERDP, EHB=0 */

    /* Run/Stop = 1。 */
    mmio_wr32(g_op + OP_USBCMD, USBCMD_RUN);
    (void)mmio_rd32(g_op + OP_USBCMD);

    for (i = 0; i < XHCI_TIMEOUT_RESET; i++) {
        if (!(mmio_rd32(g_op + OP_USBSTS) & USBSTS_HCHALTED)) break;
        cpu_pause();
    }
    if (i == XHCI_TIMEOUT_RESET) {
        serial_puts("[SRCore] xHCI: controller failed to start\n");
        return false;
    }

    g_ready = true;
    serial_puts("[SRCore] xHCI running\n");
    return true;
}

/* ============================================================
 *  7. 环、事件与命令
 * ============================================================ */

static void ring_wrap(struct xhci_ring *r) {
    struct xhci_trb *link = &r->trbs[r->size - 1];
    link->parameter = r->phys;
    link->status = 0;
    link->control = TRB_TYPE(TRB_LINK) | TRB_LINK_TC | r->cycle;
    mem_barrier();

    r->enq = 0;
    r->cycle ^= 1;
}

/* 保证接下来 n 个普通 TRB 落在 Link TRB 之前，绝不把一次 TD 拆到
   两个环段上。 */
static bool ring_ensure(struct xhci_ring *r, uint16_t n) {
    if ((uint32_t)r->enq + n > (uint32_t)(r->size - 1))
        ring_wrap(r);
    return ((uint32_t)r->enq + n <= (uint32_t)(r->size - 1));
}

static struct xhci_trb *ring_reserve(struct xhci_ring *r) {
    if ((uint32_t)r->enq + 1 > (uint32_t)(r->size - 1))
        ring_wrap(r);

    struct xhci_trb *t = &r->trbs[r->enq++];
    memset(t, 0, sizeof(*t));
    t->control = r->cycle;
    return t;
}

static bool event_pop(struct xhci_event *e) {
    if (!e) return false;

    struct xhci_trb *t = &g_evt_trbs[g_evt_index];
    load_barrier();

    uint32_t ctrl = t->control;
    if ((uint8_t)(ctrl & 1u) != g_evt_cycle) return false;

    e->parameter = t->parameter;
    e->status = t->status;
    e->control = ctrl;
    e->residual = t->status & 0xFFFFFFu;
    e->code = (uint8_t)((t->status >> 24) & 0xFFu);
    e->type = (uint8_t)((ctrl >> 10) & 0x3Fu);
    e->slot = (uint8_t)((ctrl >> 24) & 0xFFu);
    e->ep = (uint8_t)((ctrl >> 16) & 0x1Fu);

    g_evt_index++;
    if (g_evt_index >= XHCI_EVT_TRBS) {
        g_evt_index = 0;
        g_evt_cycle ^= 1;
    }

    /* 把消费者位置写回 ERDP，并写 1 清除 EHB（bit3）。 */
    uint64_t erdp = g_evt_phys + (uint64_t)g_evt_index * 16u;
    mmio_wr64(g_rt + 0x20u + 0x18u, erdp | 8u);
    return true;
}

static bool event_wait(uint8_t type, uint8_t slot, uint8_t ep,
                       struct xhci_event *out, uint32_t limit) {
    for (uint32_t i = 0; i < limit; i++) {
        struct xhci_event e;
        while (event_pop(&e)) {
            if (e.type == type &&
                (slot == 0 || e.slot == slot) &&
                (ep == 0 || e.ep == ep)) {
                if (out) *out = e;
                return true;
            }
        }
        cpu_pause();
    }
    return false;
}

static void ep_doorbell(uint8_t slot, uint8_t dci) {
    /* Doorbell Array 每个槽位一个 32 位寄存器（固定 4 字节间距）。 */
    uint32_t off = g_db + (uint32_t)slot * 4u;
    mmio_wr32(off, (uint32_t)dci);
    (void)mmio_rd32(off);            /* flush posted write */
}

/* 提交一条命令 TRB 并等待 Command Completion Event。 */
static bool xhci_command(uint8_t type, uint64_t param, uint8_t slot, bool bsr,
                         uint8_t *out_slot) {
    if (!ring_ensure(&g_cmd_ring, 1)) {
        serial_puts("[SRCore] xHCI: command ring full\n");
        return false;
    }

    struct xhci_trb *t = ring_reserve(&g_cmd_ring);
    t->parameter = param;
    t->status = 0;
    /* Command TRB 的 Slot ID 位于 control 的 31:24 位，而不是 status。 */
    t->control |= TRB_TYPE(type) | ((uint32_t)slot << 24);
    if (bsr) t->control |= (1u << 9);

    mem_barrier();
    mmio_wr32(g_db, 0);              /* Host Controller Command Doorbell */
    (void)mmio_rd32(g_db);

    struct xhci_event e;
    if (!event_wait(TRB_COMPLETION, 0, 0, &e, XHCI_TIMEOUT_CMD)) {
        serial_puts("[SRCore] xHCI: command timeout type=");
        serial_putdec(type);
        serial_puts(" usbcmd=");
        serial_puthex(mmio_rd32(g_op + OP_USBCMD));
        serial_puts(" usbsts=");
        serial_puthex(mmio_rd32(g_op + OP_USBSTS));
        serial_puts(" crcr=");
        serial_puthex(mmio_rd32(g_op + OP_CRCR));
        serial_puts(" evtctl=");
        serial_puthex(g_evt_trbs[0].control);
        serial_puts("\n");
        return false;
    }

    if (out_slot) *out_slot = e.slot;
    if (e.code != COMP_SUCCESS) {
        serial_puts("[SRCore] xHCI: command failed type=");
        serial_putdec(type);
        serial_puts(" code=");
        serial_putdec(e.code);
        serial_puts("\n");
        return false;
    }
    return true;
}

static void xhci_disable_slot(uint8_t slot) {
    if (!slot) return;
    (void)xhci_command(TRB_DISABLE_SLOT, 0, slot, false, 0);
    if (slot <= XHCI_MAX_SLOTS) g_dcbaa[slot] = 0;
}

/* ============================================================
 *  8. 上下文构造
 * ============================================================ */

static void slot_ctx_init(struct xhci_slot_ctx *s, uint8_t speed,
                          uint8_t port, uint8_t entries) {
    memset(s, 0, sizeof(*s));
    s->dw0 = SLOT_SPEED(speed) | SLOT_ENTRIES(entries);
    s->dw1 = SLOT_ROOT_PORT(port);
    s->dw2 = 0;                       /* Interrupter Target = 0 */
    s->dw3 = 0;                       /* USB 地址由控制器填写 */
}

static void ep_ctx_init(struct xhci_ep_ctx *e, uint8_t type, uint16_t maxpkt,
                        uint8_t interval, uint64_t deq) {
    memset(e, 0, sizeof(*e));
    e->dw0 = EP_INTERVAL(interval);
    e->dw1 = EP_CERR_3 | EP_TYPE(type) | EP_MAXPACKET(maxpkt);
    e->deq = deq;                     /* bit0 = DCS，必须为 1 */
    e->dw4 = 8;                       /* Average TRB Length */

    /* 周期性端点必须报告 Max ESIT Payload。对 Boot 键盘就是每包
       8 字节；控制端点保持 0。 */
    if (type == EP_TYPE_INT_IN && maxpkt) {
        e->dw0 |= ((uint32_t)(maxpkt & 0xFFu) << 24);
        e->dw4 = (uint32_t)(maxpkt & 0xFFFFu) | (8u << 16);
    }
}

static uint16_t ep0_max_for_speed(uint8_t speed) {
    switch (speed) {
    case 1: return 8;                 /* Full Speed  */
    case 2: return 8;                 /* Low Speed   */
    case 3: return 64;                /* High Speed  */
    case 4:                           /* SuperSpeed  */
    case 5: return 512;               /* SuperSpeed+ */
    default: return 8;
    }
}

static uint8_t ep_interval_for_speed(uint8_t speed, uint8_t bInterval) {
    if (!bInterval) return 0;

    if (speed >= 3) {                    /* High/SuperSpeed：2^(bInterval-1) */
        if (bInterval > 16) bInterval = 16;
        return (uint8_t)(bInterval - 1u);
    }

    /* Full/LowSpeed 的 bInterval 以帧（1ms）为单位，xHCI 的 Interval
       字段却以 125us 为单位。取最接近的 2 的幂，并限制在 1~128ms。 */
    uint32_t frames = (uint32_t)bInterval * 8u;
    uint8_t exp = 0;
    while (frames > 1u) {
        frames >>= 1;
        exp++;
    }
    if (exp < 3) exp = 3;
    if (exp > 10) exp = 10;
    return exp;
}

/* Enable Slot 必须先于 Address Device：xHCI 的 slot ID 由控制器分配，
   不是软件随便挑的。返回的 slot ID 在 Command Completion Event 的
   control[31:24]。 */
static bool xhci_enable_slot(uint8_t *slot) {
    return xhci_command(TRB_ENABLE_SLOT, 0, 0, false, slot);
}

static bool xhci_address_device(uint8_t slot, uint8_t speed, uint8_t port,
                                uint16_t ep0max) {
    if (!slot || slot > XHCI_MAX_SLOTS) return false;

    struct xhci_ring *r = &g_ep0_ring[slot - 1];
    r->trbs = g_ep0_trbs[slot - 1];
    r->phys = xhci_phys(g_ep0_trbs[slot - 1]);
    r->size = XHCI_EP_TRBS;
    r->enq = 0;
    r->cycle = 1;
    memset(r->trbs, 0, (size_t)r->size * sizeof(struct xhci_trb));
    r->trbs[r->size - 1].parameter = r->phys;
    r->trbs[r->size - 1].control =
        TRB_TYPE(TRB_LINK) | TRB_LINK_TC | TRB_CYCLE;

    memset(&g_dev_ctx[slot - 1], 0, sizeof(g_dev_ctx[slot - 1]));
    g_dcbaa[slot] = xhci_phys(&g_dev_ctx[slot - 1]);

    memset(&g_in_ctx, 0, sizeof(g_in_ctx));
    g_in_ctx.ctrl[0] = 0;                         /* Drop flags */
    g_in_ctx.ctrl[1] = CTX_SLOT | CTX_EP0;        /* Add flags  */
    slot_ctx_init(&g_in_ctx.slot, speed, port, 1);
    ep_ctx_init(&g_in_ctx.ep[0], EP_TYPE_CONTROL, ep0max, 0,
                r->phys | 1u);                    /* DCS = 1 */

    mem_barrier();
    if (!xhci_command(TRB_ADDR_DEV, xhci_phys(&g_in_ctx), slot, false, 0))
        return false;

    g_ep0_max[slot] = ep0max;
    return true;
}

/* 如果设备描述符给出的 bMaxPacketSize0 与 Address Device 时用的不同，
   用 Evaluate Context 更新 EP0 的 Max Packet Size。 */
static bool xhci_eval_ep0(uint8_t slot, uint16_t maxpkt) {
    if (!slot || slot > XHCI_MAX_SLOTS) return false;

    memset(&g_in_ctx, 0, sizeof(g_in_ctx));
    g_in_ctx.ctrl[0] = 0;
    g_in_ctx.ctrl[1] = CTX_SLOT | CTX_EP0;

    memcpy(&g_in_ctx.slot, &g_dev_ctx[slot - 1].slot,
           sizeof(g_in_ctx.slot));
    memcpy(&g_in_ctx.ep[0], &g_dev_ctx[slot - 1].ep[0],
           sizeof(g_in_ctx.ep[0]));

    g_in_ctx.slot.dw0 = (g_in_ctx.slot.dw0 & ~(0x1Fu << 27)) | SLOT_ENTRIES(1);
    g_in_ctx.ep[0].dw1 = (g_in_ctx.ep[0].dw1 & 0x0000FFFFu) |
                         EP_MAXPACKET(maxpkt);

    mem_barrier();
    if (!xhci_command(TRB_EVAL_CONTEXT, xhci_phys(&g_in_ctx), slot,
                      false, 0))
        return false;

    g_ep0_max[slot] = maxpkt;
    return true;
}

/* 初始化一条 Interrupt IN 传输环（Link TRB 固定在末尾）。 */
static void hid_ring_init(struct xhci_ring *r, struct xhci_trb *trbs) {
    r->trbs = trbs;
    r->phys = xhci_phys(trbs);
    r->size = XHCI_EP_TRBS;
    r->enq = 0;
    r->cycle = 1;
    memset(trbs, 0, (size_t)XHCI_EP_TRBS * sizeof(struct xhci_trb));
    trbs[XHCI_EP_TRBS - 1].parameter = r->phys;
    trbs[XHCI_EP_TRBS - 1].control =
        TRB_TYPE(TRB_LINK) | TRB_LINK_TC | TRB_CYCLE;
}

static bool xhci_configure_hid(struct xhci_ring *ring, uint8_t slot,
                               uint8_t dci, uint16_t maxpkt, uint8_t interval) {
    if (!ring || !slot || slot > XHCI_MAX_SLOTS || dci < 1 || dci > 31)
        return false;

    memset(&g_in_ctx, 0, sizeof(g_in_ctx));
    g_in_ctx.ctrl[0] = 0;
    g_in_ctx.ctrl[1] = CTX_SLOT | (1u << dci);

    memcpy(&g_in_ctx.slot, &g_dev_ctx[slot - 1].slot,
           sizeof(g_in_ctx.slot));
    g_in_ctx.slot.dw0 = (g_in_ctx.slot.dw0 & ~(0x1Fu << 27)) |
                        SLOT_ENTRIES(dci);

    ep_ctx_init(&g_in_ctx.ep[dci - 1], EP_TYPE_INT_IN, maxpkt, interval,
                ring->phys | 1u);

    mem_barrier();
    return xhci_command(TRB_CONFIG_EP, xhci_phys(&g_in_ctx), slot, false, 0);
}

/* ============================================================
 *  9. 控制传输与描述符
 * ============================================================ */

static int xhci_control(uint8_t slot, uint8_t bmRequestType, uint8_t bRequest,
                        uint16_t wValue, uint16_t wIndex, uint16_t wLength) {
    if (!slot || slot > XHCI_MAX_SLOTS) return -1;
    if (wLength > sizeof(g_ctrl_buf)) return -1;

    bool dir_in = (bmRequestType & 0x80u) != 0;
    bool has_data = wLength != 0;
    struct xhci_ring *r = &g_ep0_ring[slot - 1];

    uint16_t need = (uint16_t)(2u + (has_data ? 1u : 0u));
    if (!ring_ensure(r, need)) {
        serial_puts("[SRCore] xHCI: EP0 ring full\n");
        return -1;
    }

    if (has_data) memset(g_ctrl_buf, 0, sizeof(g_ctrl_buf));

    uint64_t setup = (uint64_t)bmRequestType |
                     ((uint64_t)bRequest << 8) |
                     ((uint64_t)wValue << 16) |
                     ((uint64_t)wIndex << 32) |
                     ((uint64_t)wLength << 48);

    struct xhci_trb *t = ring_reserve(r);
    t->parameter = setup;
    t->status = 8;
    t->control |= TRB_TYPE(TRB_SETUP) | TRB_IDT;
    if (has_data)
        t->control |= ((uint32_t)(dir_in ? 3u : 2u) << 16);

    if (has_data) {
        t = ring_reserve(r);
        t->parameter = xhci_phys(g_ctrl_buf);
        t->status = (uint32_t)wLength & 0x1FFFFu;
        t->control |= TRB_TYPE(TRB_DATA);
        if (dir_in) t->control |= TRB_DIR_IN;
    }

    t = ring_reserve(r);
    t->parameter = 0;
    t->status = 0;
    t->control |= TRB_TYPE(TRB_STATUS) | TRB_IOC;
    /* 数据阶段是 IN 时，状态阶段是 OUT；否则状态阶段是 IN。 */
    if (!(has_data && dir_in))
        t->control |= TRB_DIR_IN;

    mem_barrier();
    ep_doorbell(slot, 1);             /* DCI 1 = EP0 */

    struct xhci_event e;
    if (!event_wait(TRB_TRANSFER, slot, 1, &e, XHCI_TIMEOUT_XFER)) {
        serial_puts("[SRCore] xHCI: EP0 transfer timeout\n");
        return -1;
    }
    if (e.code != COMP_SUCCESS && e.code != COMP_SHORT_PACKET) {
        serial_puts("[SRCore] xHCI: EP0 transfer failed code=");
        serial_putdec(e.code);
        serial_puts("\n");
        return -1;
    }

    uint32_t actual = wLength;
    if (e.residual <= wLength) actual = (uint32_t)wLength - e.residual;
    if (actual > wLength) actual = wLength;
    return (int)actual;
}

static int xhci_get_descriptor(uint8_t slot, uint8_t type, uint8_t index,
                               uint16_t len) {
    return xhci_control(slot, 0x80, 0x06,
                        (uint16_t)(((uint16_t)type << 8) | index),
                        0, len);
}

static bool xhci_set_configuration(uint8_t slot, uint8_t config_value) {
    return xhci_control(slot, 0x00, 0x09, config_value, 0, 0) >= 0;
}

static bool xhci_set_protocol(uint8_t slot, uint8_t iface) {
    /* HID class request：SET_PROTOCOL(boot=0) */
    return xhci_control(slot, 0x21, 0x0B, 0, iface, 0) >= 0;
}

static bool xhci_set_idle(uint8_t slot, uint8_t iface) {
    /* HID class request：SET_IDLE(0,0)，不产生无意义的周期报告。 */
    return xhci_control(slot, 0x21, 0x0A, 0, iface, 0) >= 0;
}

struct hid_desc {
    bool     found;
    uint8_t  proto;          /* 1 = keyboard, 2 = mouse（Boot Protocol） */
    uint8_t  config_value;
    uint8_t  iface;
    uint8_t  ep_addr;
    uint8_t  interval;
    uint16_t maxpkt;
};

/* 解析 Configuration Descriptor，定位 Boot Protocol HID 接口
   （Class=3, SubClass=1, Protocol=1 键盘 / Protocol=2 鼠标）
   和它的 Interrupt IN 端点。 */
static bool xhci_parse_config(const uint8_t *buf, int len,
                              struct hid_desc *out) {
    if (!buf || !out || len < 9) return false;
    memset(out, 0, sizeof(*out));

    uint16_t total = (uint16_t)(buf[2] | ((uint16_t)buf[3] << 8));
    if (total < 9 || total > (uint16_t)len) total = (uint16_t)len;

    uint8_t config_value = buf[5];
    uint8_t cur_iface = 0;
    bool iface_hid = false;
    uint8_t iface_proto = 0;

    int off = 0;
    while (off + 2 <= (int)total) {
        uint8_t blen = buf[off];
        uint8_t btype = buf[off + 1];
        if (blen < 2 || off + blen > (int)total) break;

        if (btype == 4 && blen >= 9) {         /* INTERFACE */
            cur_iface = buf[off + 2];
            uint8_t alt = buf[off + 3];
            uint8_t cls = buf[off + 5];
            uint8_t sub = buf[off + 6];
            uint8_t proto = buf[off + 7];
            iface_hid = (alt == 0 && cls == 3 && sub == 1 &&
                         (proto == 1 || proto == 2));
            iface_proto = proto;
        } else if (btype == 5 && blen >= 7 && iface_hid) {
            uint8_t addr = buf[off + 2];
            uint8_t attr = buf[off + 3];
            uint16_t mps = (uint16_t)(buf[off + 4] |
                                      ((uint16_t)buf[off + 5] << 8));
            if ((addr & 0x80u) && ((attr & 0x03u) == 0x03u)) {
                out->found = true;
                out->proto = iface_proto;
                out->config_value = config_value;
                out->iface = cur_iface;
                out->ep_addr = addr;
                out->maxpkt = (uint16_t)(mps & 0x07FFu);
                if (!out->maxpkt) out->maxpkt = 8;
                out->interval = buf[off + 6];
                return true;
            }
        }

        off += blen;
    }
    return false;
}

/* ============================================================
 * 10. HID 报告处理
 * ============================================================ */

static void hid_usage_key(uint8_t usage, bool shift) {
    uint8_t c = 0;

    if (usage >= 0x04 && usage <= 0x1D) {           /* a-z */
        c = (uint8_t)('a' + (usage - 0x04));
        if (shift ^ g_kbd_caps) c = (uint8_t)(c - 'a' + 'A');
    } else if (usage >= 0x1E && usage <= 0x26) {    /* 1-9 */
        c = (uint8_t)('1' + (usage - 0x1E));
        if (shift) {
            static const char shifted[] = "!@#$%^&*(";
            c = (uint8_t)shifted[usage - 0x1E];
        }
    } else if (usage == 0x27) {                     /* 0 */
        c = shift ? ')' : '0';
    } else {
        switch (usage) {
        case 0x28: c = '\n'; break;                 /* Enter */
        case 0x29: c = 0x1B; break;                 /* Esc */
        case 0x2A: c = '\b'; break;                 /* Backspace */
        case 0x2B: c = '\t'; break;                 /* Tab */
        case 0x2C: c = ' '; break;                  /* Space */
        case 0x2D: c = shift ? '_' : '-'; break;
        case 0x2E: c = shift ? '+' : '='; break;
        case 0x2F: c = shift ? '{' : '['; break;
        case 0x30: c = shift ? '}' : ']'; break;
        case 0x31: c = shift ? '|' : '\\'; break;
        case 0x32: c = shift ? ':' : ';'; break;
        case 0x33: c = shift ? '"' : '\''; break;
        case 0x34: c = shift ? '~' : '`'; break;
        case 0x35: c = shift ? '<' : ','; break;
        case 0x36: c = shift ? '>' : '.'; break;
        case 0x37: c = shift ? '?' : '/'; break;
        case 0x38: c = 0; break;                    /* Caps Lock 单独处理 */
        case 0x4A: input_post_key(KEY_HOME); return;
        case 0x4B: input_post_key(KEY_PGUP); return;
        case 0x4C: input_post_key(KEY_DELETE); return;
        case 0x4D: input_post_key(KEY_END); return;
        case 0x4E: input_post_key(KEY_PGDN); return;
        case 0x4F: input_post_key(KEY_RIGHT); return;
        case 0x50: input_post_key(KEY_LEFT); return;
        case 0x51: input_post_key(KEY_DOWN); return;
        case 0x52: input_post_key(KEY_UP); return;
        default: return;
        }
    }

    if (c) input_post_key((uint16_t)c);
}

static void hid_process_report(const uint8_t *r) {
    uint8_t mods = r[0];
    bool shift = (mods & 0x22u) != 0;
    keyboard_set_usb_ctrl((mods & 0x11u) != 0);

    uint8_t *prev = g_hid[HID_DEV_KBD].prev;
    uint8_t now[6];
    for (int i = 0; i < 6; i++) now[i] = r[2 + i];

    for (int i = 0; i < 6; i++) {
        uint8_t k = now[i];
        if (!k) continue;

        bool was = false;
        for (int j = 0; j < 6; j++)
            if (prev[j] == k) { was = true; break; }
        if (was) continue;

#if XHCI_DEBUG
        serial_puts("[SRCore] xHCI HID usage=");
        serial_puthex(k);
        serial_puts("\n");
#endif

        if (k == 0x39) {                  /* Caps Lock：按下沿翻转 */
            g_kbd_caps = !g_kbd_caps;
        } else {
            hid_usage_key(k, shift);
        }
    }

    for (int i = 0; i < 6; i++) prev[i] = now[i];
}

/* HID Boot Mouse 报告（3 字节）：
     r[0] = 按键位（bit0 左键、bit1 右键、bit2 中键）
     r[1] = X 位移（有符号，右为正）
     r[2] = Y 位移（有符号，上为正，与 input 事件约定一致）
   统一投递到输入子系统，与 PS/2 鼠标走同一条队列（含登录 overlay）。 */
static void hid_process_mouse_report(const uint8_t *r) {
    uint8_t buttons = (uint8_t)(r[0] & 0x07u);
    int32_t dx = (int32_t)(int8_t)r[1];
    int32_t dy = (int32_t)(int8_t)r[2];

    input_post_mouse(buttons, dx, dy);
}

/* ============================================================
 * 11. 端口复位、枚举、HID 设备激活
 * ============================================================ */

static bool hid_arm(struct hid_dev *d);

static uint32_t port_read(uint8_t port) {
    return mmio_rd32(g_op + OP_PORTSC_BASE +
                     ((uint32_t)port - 1u) * PORTSC_SIZE);
}
static void port_write(uint8_t port, uint32_t v) {
    mmio_wr32(g_op + OP_PORTSC_BASE +
              ((uint32_t)port - 1u) * PORTSC_SIZE, v);
}
static uint32_t port_neutral(uint32_t sc) {
    return sc & PORTSC_KEEP;
}

static bool xhci_port_reset(uint8_t port) {
    uint32_t sc = port_read(port);
    if (!(sc & PORTSC_CCS)) return false;

    /* 打开端口电源。某些控制器 PP 只读或已由固件置位。 */
    if (!(sc & PORTSC_PP)) {
        port_write(port, port_neutral(sc) | PORTSC_PP);
        for (uint32_t i = 0; i < 200000u; i++) cpu_pause();
        sc = port_read(port);
    }

    /* 先清掉所有 RW1C 变更位，避免旧状态干扰。 */
    port_write(port, port_neutral(sc) | (sc & PORTSC_RWC));
    sc = port_read(port);

    /* 写 PR 发起端口复位，保留 PP/PIC 等 RWS 位。 */
    port_write(port, port_neutral(sc) | PORTSC_PR);

    uint32_t i;
    for (i = 0; i < XHCI_TIMEOUT_PORT; i++) {
        sc = port_read(port);
        if ((sc & PORTSC_PRC) && !(sc & PORTSC_PR)) break;
        cpu_pause();
    }
    if (i == XHCI_TIMEOUT_PORT) {
        serial_puts("[SRCore] xHCI: port reset timeout\n");
        return false;
    }

    /* 清 PRC（会同时清除其他 RW1C 位；写 1 清零）。 */
    sc = port_read(port);
    port_write(port, port_neutral(sc) | PORTSC_PRC);

    /* 等链路进入 U0 并且端口使能。 */
    for (i = 0; i < XHCI_TIMEOUT_PORT; i++) {
        sc = port_read(port);
        if (((sc & PORTSC_PLS_MASK) == 0) && (sc & PORTSC_PED))
            return true;
        cpu_pause();
    }

    serial_puts("[SRCore] xHCI: port did not enter U0\n");
    return false;
}

/* 把枚举出来的 Boot Protocol HID 设备挂到驱动状态上：配置 Interrupt IN
   端点 → SET_CONFIGURATION → SET_PROTOCOL(boot) → SET_IDLE → arm。
   键盘与鼠标各用独立的 slot / DCI / 传输环，互不影响。 */
static bool xhci_activate_hid(uint8_t port, uint8_t slot, uint8_t speed,
                              const struct hid_desc *hd) {
    int kind = (hd->proto == 2) ? HID_DEV_MOUSE : HID_DEV_KBD;
    struct hid_dev *d = &g_hid[kind];

    if (d->ready) {
        serial_puts("[SRCore] xHCI: duplicate HID device skipped\n");
        return false;
    }

    uint8_t dci = (uint8_t)(((hd->ep_addr & 0x0Fu) << 1) |
                            ((hd->ep_addr & 0x80u) ? 1u : 0u));
    uint8_t interval = ep_interval_for_speed(speed, hd->interval);

    hid_ring_init(&d->ring, d->trbs);

    if (!xhci_configure_hid(&d->ring, slot, dci, hd->maxpkt, interval)) {
        serial_puts("[SRCore] xHCI: configure HID endpoint failed\n");
        return false;
    }

    if (!xhci_set_configuration(slot, hd->config_value)) {
        serial_puts("[SRCore] xHCI: SET_CONFIGURATION failed\n");
        return false;
    }

    /* Boot Protocol。失败也不致命：很多设备默认 report protocol 的
       前几个字节与 boot report 完全一致。 */
    if (!xhci_set_protocol(slot, hd->iface))
        serial_puts("[SRCore] xHCI: SET_PROTOCOL(boot) failed\n");
    (void)xhci_set_idle(slot, hd->iface);

    d->proto   = hd->proto;
    d->slot    = slot;
    d->dci     = dci;
    d->iface   = hd->iface;
    d->ep_addr = hd->ep_addr;
    d->port    = port;
    memset(d->buf, 0, 64);
    memset(d->prev, 0, sizeof(d->prev));
    d->ready = true;
    d->armed = false;

    if (kind == HID_DEV_KBD) {
        g_kbd_caps = false;
        keyboard_set_usb_ctrl(false);
    }

    g_last_poll_tick = timer_ticks();

    if (!hid_arm(d)) {
        if (kind == HID_DEV_KBD) keyboard_set_usb_ctrl(false);
        d->ready = false;
        d->slot = 0;
        serial_puts("[SRCore] xHCI: failed to arm HID endpoint\n");
        return false;
    }

    serial_puts("[SRCore] xHCI HID ");
    serial_puts(kind == HID_DEV_MOUSE ? "mouse" : "keyboard");
    serial_puts(" ready slot=");
    serial_putdec(slot);
    serial_puts(" port=");
    serial_putdec(port);
    serial_puts(" iface=");
    serial_putdec(hd->iface);
    serial_puts(" ep=");
    serial_puthex(hd->ep_addr);
    serial_puts("\n");
    return true;
}

/* 对一个已连接的根端口做完整枚举。成功激活键盘或鼠标返回 true。 */
static bool xhci_try_port(uint8_t port) {
    if (!xhci_port_reset(port)) return false;

    uint32_t sc = port_read(port);
    uint8_t speed = (uint8_t)((sc & PORTSC_SPEED_MASK) >> 10);
    if (speed == 0 || speed > 5) {
        serial_puts("[SRCore] xHCI: unknown port speed\n");
        return false;
    }

    uint8_t slot = 0;
    if (!xhci_enable_slot(&slot)) return false;
    if (!slot || slot > XHCI_MAX_SLOTS) {
        serial_puts("[SRCore] xHCI: controller returned slot ");
        serial_putdec(slot);
        serial_puts(" (driver cap is 16)\n");
        if (slot) xhci_disable_slot(slot);
        return false;
    }

    uint16_t ep0max = ep0_max_for_speed(speed);
    if (!xhci_address_device(slot, speed, port, ep0max)) {
        xhci_disable_slot(slot);
        return false;
    }

    /* 先读设备描述符前 8 字节，拿到真实的 bMaxPacketSize0。
       FS/LS 默认控制端点可能只有 8 字节，不能一上来就假设 64。 */
    int n = xhci_get_descriptor(slot, 1, 0, 8);
    if (n < 8) {
        xhci_disable_slot(slot);
        return false;
    }

    uint8_t mps = g_ctrl_buf[7];
    if (mps == 9) mps = 0;                 /* USB3 用 9 表示 512 */
    if (mps == 8 || mps == 16 || mps == 32 || mps == 64) {
        if (mps != ep0max) {
            if (xhci_eval_ep0(slot, mps))
                ep0max = mps;
            else
                serial_puts("[SRCore] xHCI: evaluate EP0 failed\n");
        }
    }

    n = xhci_get_descriptor(slot, 1, 0, 18);
    if (n < 18) {
        xhci_disable_slot(slot);
        return false;
    }

    uint16_t vid = (uint16_t)(g_ctrl_buf[8] | ((uint16_t)g_ctrl_buf[9] << 8));
    uint16_t pid = (uint16_t)(g_ctrl_buf[10] | ((uint16_t)g_ctrl_buf[11] << 8));
    uint8_t dev_class = g_ctrl_buf[4];

    serial_puts("[SRCore] xHCI device slot=");
    serial_putdec(slot);
    serial_puts(" port=");
    serial_putdec(port);
    serial_puts(" speed=");
    serial_putdec(speed);
    serial_puts(" VID=");
    serial_puthex(vid);
    serial_puts(" PID=");
    serial_puthex(pid);
    serial_puts("\n");

    /* 根端口后面如果是 Hub，本驱动不做多级枚举；直接释放槽位。 */
    if (dev_class == 9) {
        serial_puts("[SRCore] xHCI: hub skipped (only root-port devices)\n");
        xhci_disable_slot(slot);
        return false;
    }

    n = xhci_get_descriptor(slot, 2, 0, sizeof(g_ctrl_buf));
    if (n < 9) {
        xhci_disable_slot(slot);
        return false;
    }

    struct hid_desc hd;
    if (!xhci_parse_config(g_ctrl_buf, n, &hd)) {
        xhci_disable_slot(slot);
        return false;
    }

    if (!xhci_activate_hid(port, slot, speed, &hd)) {
        xhci_disable_slot(slot);
        return false;
    }
    return true;
}

/* 触发一条 8 字节的 Interrupt IN 传输。完成事件由
   xhci_poll_keyboard() 消费，处理完报告后重新 arm，形成轮询闭环。 */
static bool hid_arm(struct hid_dev *d) {
    if (!d || !d->ready) return false;
    if (!ring_ensure(&d->ring, 1)) return false;

    memset(d->buf, 0, HID_REPORT_MAX);
    struct xhci_trb *t = ring_reserve(&d->ring);
    t->parameter = xhci_phys(d->buf);
    t->status = HID_REPORT_MAX;
    t->control |= TRB_TYPE(TRB_NORMAL) | TRB_IOC;

    mem_barrier();
    ep_doorbell(d->slot, d->dci);
    d->armed = true;
#if XHCI_DEBUG
    serial_puts("[SRCore] xHCI HID armed slot=");
    serial_putdec(d->slot);
    serial_puts(" dci=");
    serial_putdec(d->dci);
    serial_puts(" trb=");
    serial_puthex(t->parameter);
    serial_puts(" len=");
    serial_putdec(t->status);
    serial_puts(" ctrl=");
    serial_puthex(t->control);
    serial_puts("\n");
#endif
    return true;
}

/* ============================================================
 * 12. 对外接口
 * ============================================================ */

/* 控制器复位后，把所有 HID 状态清零（保留静态 DMA 指针）。 */
static void hid_reset_state(void) {
    for (int i = 0; i < HID_DEV_COUNT; i++) {
        g_hid[i].ready = false;
        g_hid[i].armed = false;
        g_hid[i].slot  = 0;
        g_hid[i].dci   = 0;
        g_hid[i].port  = 0;
        memset(g_hid[i].prev, 0, sizeof(g_hid[i].prev));
        memset(g_hid[i].buf, 0, 64);
    }
    g_port_failed = 0;
    g_last_scan_tick = 0;
}

/* 重新标记端口：清掉 RW1C 变更位，避免旧状态被当成新插入。 */
static void port_clear_changes(uint8_t port) {
    uint32_t sc = port_read(port);
    if (sc & PORTSC_RWC)
        port_write(port, port_neutral(sc) | (sc & PORTSC_RWC));
}

/* 扫描所有根端口：给“还没有设备”的端口做一次枚举。
   - 已经挂着键盘/鼠标的端口直接跳过，避免复位正在工作的设备；
   - 枚举失败过的端口记入 g_port_failed，设备拔掉后自动解除；
   - 键盘和鼠标都就绪后提前结束。 */
static bool xhci_scan_ports(void) {
    bool any = false;

    for (uint8_t p = 1; p <= g_max_ports; p++) {
        if (g_hid[HID_DEV_KBD].ready && g_hid[HID_DEV_MOUSE].ready) break;

        if ((g_hid[HID_DEV_KBD].ready && g_hid[HID_DEV_KBD].port == p) ||
            (g_hid[HID_DEV_MOUSE].ready && g_hid[HID_DEV_MOUSE].port == p))
            continue;

        uint32_t sc = port_read(p);
        if (!(sc & PORTSC_CCS)) {
            g_port_failed &= ~(1u << p);        /* 已拔掉：下次插入可重试 */
            continue;
        }
        if (g_port_failed & (1u << p)) continue;

#if XHCI_DEBUG
        serial_puts("[SRCore] xHCI probing port ");
        serial_putdec(p);
        serial_puts(" PORTSC=");
        serial_puthex(sc);
        serial_puts("\n");
#endif

        port_clear_changes(p);
        if (xhci_try_port(p)) {
            any = true;
            g_port_failed &= ~(1u << p);
        } else {
            g_port_failed |= 1u << p;
        }
    }
    return any;
}

bool xhci_kbd_init(void) {
    if (!xhci_init()) return false;
    if (g_hid[HID_DEV_KBD].ready || g_hid[HID_DEV_MOUSE].ready)
        return g_hid[HID_DEV_KBD].ready;

    hid_reset_state();
    xhci_scan_ports();
    g_last_scan_tick = timer_ticks();

    if (!g_hid[HID_DEV_KBD].ready)
        serial_puts("[SRCore] xHCI: no boot HID keyboard found\n");
    return g_hid[HID_DEV_KBD].ready;
}

void xhci_poll_keyboard(void) {
    if (!g_ready) return;

    uint64_t now = timer_ticks();
    if (now == g_last_poll_tick) return;
    g_last_poll_tick = now;

    struct xhci_event e;
    while (event_pop(&e)) {
#if XHCI_DEBUG
        serial_puts("[SRCore] xHCI event type=");
        serial_putdec(e.type);
        serial_puts(" slot=");
        serial_putdec(e.slot);
        serial_puts(" ep=");
        serial_putdec(e.ep);
        serial_puts(" code=");
        serial_putdec(e.code);
        serial_puts(" residual=");
        serial_putdec(e.residual);
        serial_puts("\n");
#endif
        if (e.type != TRB_TRANSFER) continue;

        /* 事件按 (slot, dci) 找到对应的键盘/鼠标。 */
        int kind = -1;
        for (int i = 0; i < HID_DEV_COUNT; i++) {
            if (g_hid[i].ready && e.slot == g_hid[i].slot &&
                e.ep == g_hid[i].dci) {
                kind = i;
                break;
            }
        }
        if (kind < 0) continue;

        struct hid_dev *d = &g_hid[kind];
        uint32_t actual = HID_REPORT_MAX;
        if (e.residual <= HID_REPORT_MAX)
            actual = HID_REPORT_MAX - e.residual;

        /* Boot 报告：键盘 8 字节（mods + 保留 + 6 键），鼠标 3 字节。 */
        bool ok = (e.code == COMP_SUCCESS || e.code == COMP_SHORT_PACKET);
        if (ok && actual >= (kind == HID_DEV_KBD ? 8u : 3u)) {
            if (kind == HID_DEV_KBD) hid_process_report(d->buf);
            else                     hid_process_mouse_report(d->buf);
        } else if (!ok) {
            serial_puts("[SRCore] xHCI HID transfer error code=");
            serial_putdec(e.code);
            serial_puts("\n");
        }

        d->armed = false;

        /* STALL 说明端点已停；不要继续往一个 halted 端点打 doorbell，
           释放槽位后交给热插拔扫描重新枚举。 */
        if (e.code == COMP_STALL) {
            if (kind == HID_DEV_KBD) keyboard_set_usb_ctrl(false);
            d->ready = false;
            d->slot  = 0;
            xhci_disable_slot(e.slot);
            continue;
        }

        if (!hid_arm(d)) {
            if (kind == HID_DEV_KBD) keyboard_set_usb_ctrl(false);
            d->ready = false;
            d->slot  = 0;
        }
    }

    /* 低频热插拔：任一设备缺失时约每 1s 重扫一次根端口
       （timer 1 tick = 10ms，所以 100 tick）。 */
    if ((!g_hid[HID_DEV_KBD].ready || !g_hid[HID_DEV_MOUSE].ready) &&
        (now - g_last_scan_tick) >= 100) {
        g_last_scan_tick = now;
        xhci_scan_ports();
    }
}

bool xhci_kbd_present(void) {
    return g_hid[HID_DEV_KBD].ready;
}

bool xhci_mouse_present(void) {
    return g_hid[HID_DEV_MOUSE].ready;
}

bool xhci_present(void) {
    return g_ready;
}
