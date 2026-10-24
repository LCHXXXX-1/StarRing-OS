#include <stdint.h>
#include <limine.h>
#include "drivers/framebuffer.h"
#include "drivers/serial.h"
#include "drivers/pic.h"
#include "drivers/keyboard.h"
#include "drivers/input.h"
#include "drivers/timer.h"
#include "drivers/mouse.h"
#include "drivers/usb/xhci.h"
#include "gfx/gfx.h"
#include "gfx/font.h"
#include "gfx/console.h"
#include "gfx/gui.h"
#include "gfx/splash.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/tss.h"
#include "arch/x86_64/syscall.h"
#include "arch/x86_64/cpuinfo.h"
#include "hw/sysinfo.h"
#include "fs/fs.h"
#include "fs/partition.h"
#include "apps/terminal.h"
#include "apps/login.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/apic.h"
#include "mm/heap.h"
#include "gfx/font_ttf.h"
#include "task/task.h"
#include "lib/string.h"
#include "boot/limine_requests.h"

/* ---- 实体机无串口排查：屏幕左上角两位数步骤标记 ----
   只写 backbuffer 再单独上屏那 22x18 小块，不依赖 console / 中断 / 字体引擎
   （8x16 点阵是编译进内核的静态数据）。编号 = 已完成的步骤，见 kmain。 */
static void step_mark(uint8_t n) {
    char s[3];
    s[0] = (char)('0' + n / 10);
    s[1] = (char)('0' + n % 10);
    s[2] = 0;
    gfx_rect(2, 2, 22, 18, 0x00000000);
    font_draw_str(4, 3, s, 0x00FF3030);
    gfx_present_rect(2, 2, 22, 18);
}

/* ---- 实体机输入排查：屏幕设备状态标记 ----
   只写 backbuffer 再单独上屏那一小块，不依赖 console / 中断 / TTF。
   注意：gui_init() 的第一次全屏合成会覆盖这些标记，
   所以它们只在“开机到进桌面”之间可见，烧盘后请盯着启动过程看。 */
static void dev_mark(int y, const char *s, uint32_t col) {
    gfx_rect(2, y, 260, 16, 0x00000000);
    font_draw_str(4, y + 1, s, col);
    gfx_present_rect(2, y, 260, 16);
}

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(6);

/* 所有 Limine 请求只在 kernel/boot/limine_requests.c 里定义一次
   （Limine 对同一 request ID 出现多次会直接 PANIC:
    "Conflict detected for request ID ..."）。这里只通过 extern 取用：
      g_fb_req      → 帧缓冲
      g_memmap_req  → 物理内存布局
      g_hhdm_req    → 由 mm/vmm.c 通过 vmm_hhdm_offset() 提供给其它模块 */

/* 开机不再向终端打印 banner：登录完成直接进桌面。
   系统信息改为桌面右键“此电脑 → 属性”（apps/sysprop.c）。 */

/* ---- 开机写盘自检：验证 AHCI 写路径 + SRFS 元数据更新 ----
   全新空盘的完整链路是：partition_scan() → fs_mount_all() → mkfs()
   → fs_bootstrap() 建目录树，这里再额外写一个 /hello.txt 并读回逐字节比对，
   任何一步失败都打印原因，避免“写命令假成功、盘上其实没内容”这类问题
   被静默吞掉。 */
static void fs_write_selftest(void) {
    static const char msg[] = "test";      /* 正好 4 字节 */
    char back[16];

    if (!fs_ready()) {
        serial_puts("[SRCore] selftest: skipped, no filesystem\n");
        return;
    }

    serial_puts("[SRCore] selftest: writing /hello.txt (4 bytes)...\n");
    int w = fs_write_file("/hello.txt", msg, 4);
    if (w < 0) {
        serial_puts("[SRCore] selftest: WRITE FAILED: ");
        serial_puts(fs_strerror(w));
        serial_puts("\n");
        return;
    }
    if (w != 4) {
        serial_puts("[SRCore] selftest: short write, only ");
        serial_putdec((uint64_t)w);
        serial_puts(" bytes\n");
        return;
    }

    /* 读回比对：确认数据真落到了磁盘，而且 inode/块映射也建对了 */
    for (int i = 0; i < 16; i++) back[i] = 0;
    int r = fs_read("/hello.txt", back, sizeof(back) - 1, 0);
    if (r != 4) {
        serial_puts("[SRCore] selftest: READ BACK FAILED: ");
        serial_puts(r < 0 ? fs_strerror(r) : "unexpected length");
        serial_puts("\n");
        return;
    }
    if (kstrcmp(back, "test") != 0) {
        serial_puts("[SRCore] selftest: CONTENT MISMATCH, got '");
        serial_puts(back);
        serial_puts("'\n");
        return;
    }

    /* 目录项也要落盘，否则 ls 看不见这个文件 */
    struct fs_dirent ents[FS_MAX_DIRENTS];
    int n = fs_listdir("/", ents, FS_MAX_DIRENTS);
    serial_puts("[SRCore] selftest: PASS, /hello.txt == \"test\", root entries=");
    serial_putdec(n < 0 ? 0 : (uint64_t)n);
    serial_puts("\n");
}

void kmain(void) {
    serial_init();
    serial_puts("[SRCore] booting...\n");

    if (!g_fb_req.response) {
        serial_puts("[SRCore] FATAL: no framebuffer response\n");
        for (;;) hlt();
    }
    if (!fb_init(g_fb_req.response)) {
        serial_puts("[SRCore] FATAL: unsupported framebuffer format\n");
        for (;;) hlt();
    }
    if (!gfx_init()) {
        serial_puts("[SRCore] FATAL: gfx init failed\n");
        for (;;) hlt();
    }
    serial_puts("[SRCore] gfx ok, fb=");
    serial_putdec(fb_width());
    serial_puts("x");
    serial_putdec(fb_height());
    serial_puts(" bpp=");
    serial_putdec(fb_bpp());
    serial_puts(" pitch=");
    serial_putdec(fb_pitch());
    serial_puts(" addr=");
    serial_puthex((uint64_t)(uintptr_t)(fb_get() ? fb_get()->address : 0));
    serial_puts(" bytes=");
    serial_putdec((uint64_t)fb_pitch() * fb_height());
    serial_puts(" logical=");
    serial_putdec(gfx_width());
    serial_puts("x");
    serial_putdec(gfx_height());
    serial_puts("\n");

    /* GDT/TSS/IDT 必须在开机动画之前装好：动画会直接写帧缓冲，万一
       真机帧缓冲映射异常，至少能走异常处理而不是三重故障重启。
       这两个初始化不依赖 PMM/VMM。 */
    tss_init();
    serial_puts("[SRCore] tss ok\n");

    idt_init();
    serial_puts("[SRCore] idt ok\n");

    /* ---- 开机动画尽早显示 ----
       不读磁盘、不依赖 Limine module（没有就用内核内置图片），
       因此从 Ventoy/Live USB/空盘启动也能立即看到画面。
       动画结束后画面保留到 gui_init() 第一次全屏合成。 */
    splash_show();
    step_mark(1);

    cpuinfo_init();
    step_mark(2);
    if (g_memmap_req.response) {
        meminfo_init(g_memmap_req.response);
        if (pmm_init(g_memmap_req.response))
            serial_puts("[SRCore] pmm ok\n");
        else
            serial_puts("[SRCore] pmm unavailable\n");
    } else {
        serial_puts("[SRCore] no memmap, pmm skipped\n");
    }
    step_mark(3);

    {
        if (vmm_init(vmm_hhdm_offset()))
            serial_puts("[SRCore] vmm ok\n");
        else
            serial_puts("[SRCore] vmm unavailable\n");
    }
    step_mark(4);

    if (heap_init())
        serial_puts("[SRCore] heap ok\n");
    else
        serial_puts("[SRCore] heap unavailable\n");
    step_mark(5);

    task_init();                              /* 任务登记表 */
    task_add("kernel", TASK_KIND_SYSTEM, 0);
    serial_puts("[SRCore] hw info ok\n");
    step_mark(6);

    /* SYSCALL/SYSRET 的 MSR 配置（依赖 GDT 选择子） */
    syscall_init();
    serial_puts("[SRCore] syscall ok\n");
    step_mark(7);

    /* 输入子系统：所有驱动（PS/2、xHCI）统一向这个队列投递事件，
       GUI/登录只从队列消费。必须在任何驱动注册中断之前初始化。 */
    input_init();

    /* Phase 2：先建立 LAPIC + IOAPIC 路由，再重定位 8259。
       - 成功：ISA IRQ0/1/12 走 IOAPIC；随后整片屏蔽 8259，避免同一条
         中断既走 IOAPIC 又走 LINT0 ExtINT（时钟翻倍、按键重复）。
       - 失败：apic_disable() 回到“8259 直连 CPU”的基线路径，保证 QEMU
         在异常平台上的行为不回归。 */
    bool ioapic_ok = apic_init() && ioapic_init();
    if (!ioapic_ok) apic_disable();

    pic_remap();
    serial_puts("[SRCore] pic ok\n");
    if (ioapic_ok) {
        pic_disable();
        serial_puts("[SRCore] irq: ioapic mode\n");
    } else {
        serial_puts("[SRCore] irq: pic mode (fallback)\n");
    }
    step_mark(8);

    keyboard_init();
    serial_puts("[SRCore] kbd ok\n");
    step_mark(9);
    dev_mark(40, "kbd:ps2 init", 0x00FFCC00);
    timer_init();
    serial_puts("[SRCore] timer ok\n");
    step_mark(10);

    /* PS/2 鼠标（IRQ12）：与键盘共用 i8042，初始化完再打开 IRQ12。 */
    mouse_init();
    serial_puts("[SRCore] mouse ok\n");
    step_mark(11);

    /* 可选 USB：一次枚举同时识别 HID 键盘和 HID 鼠标；找不到 xHCI
       控制器时静默返回（PCI 扫描有界，不会卡死），PS/2 键鼠继续工作。
       热插拔由 xhci_poll_keyboard() 里的周期扫描接管。 */
    bool usb_kbd = xhci_kbd_init();
    bool usb_mouse = xhci_mouse_present();
    dev_mark(58, usb_kbd ? "kbd:usb ok" : "kbd:usb none",
             usb_kbd ? 0x0030FF30 : 0x00FF3030);
    dev_mark(76, usb_mouse ? "mouse:usb ok" : "mouse:usb none",
             usb_mouse ? 0x0030FF30 : 0x00FF3030);
    if (usb_kbd)
        serial_puts("[SRCore] xhci kbd ok\n");
    if (usb_mouse)
        serial_puts("[SRCore] xhci mouse ok\n");
    step_mark(12);

    console_init();
    serial_puts("[SRCore] console ok\n");
    step_mark(13);

    /* 多盘/多分区启动：扫描所有 AHCI 盘的 MBR，把每个 0x53/0x83 分区
       交给 VFS。第一个可挂载分区作为根 "/"，其余挂到 /mnt/<bdev>。
       只有 disk0 完全空白时才自动创建 MBR，避免误初始化用户的数据盘。 */
    bool scanned = partition_scan();
    if (scanned) {
        if (!partition_find_srfs())
            serial_puts("[SRCore] no SRFS (0x53) or Linux (0x83) partition found\n");
    } else {
        serial_puts("[SRCore] no usable MBR partition table\n");
        if (partition_disk_is_blank(0)) {
            serial_puts("[SRCore] blank disk: auto-creating MBR + SRFS at LBA 2048\n");
            if (partition_create_mbr(0, PART_DEFAULT_START, PART_TYPE_SRFS))
                serial_puts("[SRCore] blank disk: MBR created\n");
            else
                serial_puts("[SRCore] blank disk: MBR creation FAILED\n");
        }
    }
    step_mark(14);

    bool fs_mounted = (fs_mount_all() > 0);
    if (fs_mounted) {
        serial_puts("[SRCore] fs ok\n");
    } else {
        serial_puts("[SRCore] fs unavailable\n");
    }
    step_mark(15);

    if (fs_mounted)
        fs_write_selftest();      /* 开机自动验证 AHCI 写盘 + SRFS 元数据 */
    step_mark(16);

    /* 字体：ISO 把 siyuan.sfn 作为 Limine module 载入内存（见 limine.conf）。
       - 有盘但盘上没有字体（VMware 自建空盘：fs_bootstrap 只建目录不写字体）
         → font_provision() 把 module 里的字体一次性落盘，幂等
       - 有盘且有字体 → ttf_init_default() 照旧读 /etc/system.srs 的 font_path
       - 无盘（Live CD）→ 直接以 module 内存为字体，零拷贝、不分配 */
    bool font_ok;
    if (fs_ready()) {
        font_provision();
        font_ok = ttf_init_default();
        /* 盘写不进去（空间不足/外来文件系统）也不能丢字：直接吃 module 内存 */
        if (!font_ok) font_ok = ttf_init_module();
    } else {
        font_ok = ttf_init_module();
    }
    if (font_ok) {
        serial_puts("[SRCore] ttf font loaded\n");
        ttf_selftest();           /* 串口留证：分辨率 + ASCII/汉字真实墨水像素 */
    } else {
        serial_puts("[SRCore] ttf unavailable, using 8x16 bitmap\n");
    }
    step_mark(17);

    gui_init();               /* 第一帧全屏合成会覆盖仍在屏幕上的开机动画 */
    serial_puts("[SRCore] gui ok\n");
    step_mark(18);

    task_add("desktop", TASK_KIND_SYSTEM, 0);

    /* ---- 全屏登录 ----
       login_run() 内部临时 sti，使 PIT/鼠标/键盘在阻塞登录期间可用；
       认证成功后清 login hooks 直接进桌面：不再弹终端、不再打印 banner。 */
    dev_mark(94, "entering login", 0x00CCCCCC);
    login_run();
    step_mark(19);

    /* ---- Ring 3 bring-up 演示 ----
       开机不再自动运行：没有终端窗口时它们只在后台占用时间。
       需要验证 Ring 3 时从 Shell 里执行 ring3 / ring3probe 即可。 */
    /* if (usermode_run_demo())
        serial_puts("[SRCore] ring3 demo ok\n");
    else
        serial_puts("[SRCore] ring3 demo FAILED\n"); */

    /* 越权探针同样改为手动触发。 */
    /* if (usermode_run_probe())
        serial_puts("[SRCore] ring3 isolation ok\n");
    else
        serial_puts("[SRCore] ring3 isolation FAILED\n"); */

    /* 预置一行提示：之后从开始菜单打开终端时，网格里已有 PROMPT 可显示 */
    terminal_prompt();
    serial_puts("[SRCore] desktop ready\n");

    __asm__ volatile ("sti");
    serial_puts("[SRCore] sti ok, entering desktop loop\n");

    gui_run();                /* 桌面事件主循环，不返回 */
}
