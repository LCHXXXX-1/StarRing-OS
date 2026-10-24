# StarRing OS

StarRing OS 是一个从零实现的 **x86_64 爱好型操作系统**：自研内核 **SRCore**、自研磁盘文件系统 **SRFS**、一套全中文的图形桌面，以及 Ring 3 用户态隔离验证。不依赖 libc / POSIX，内核以静态内存 + 轮询驱动为主，目标是"能在普通 PC（或 QEMU / VMware）上完整跑起来"。

| 项 | 值 |
| --- | --- |
| 当前版本 | `0.1.0`（`/etc/version`） |
| 内核名 | SRCore |
| 架构名 | Ring Architecture（Ring 0 / Ring 3） |
| 文件系统 | SRFS（类 ext2，4 KiB 块） |
| 配置后缀 | `.srs`（如 `/etc/system.srs`） |
| 模块 / 归档后缀 | `.srm` / `.sr`（规划中） |
| 引导 | Limine，BIOS + UEFI；内核位于高半区 `0xffffffff80000000` |
| 语言 | C（freestanding）+ NASM |
| 规模 | 49 个 `.c`、3 个 `.asm`、55 个头文件 |
| 运行形态 | 单核、单线程事件循环 + 中断；无抢占调度器 |

> 教学 / 实验项目：代码里有大量中文注释，解释"为什么这么写"。界面与 Shell 输出已全量中文化。

---

## 目录

- [功能总览](#功能总览)
- [设计取舍](#设计取舍)
- [快速开始](#快速开始)
- [运行](#运行)
- [目录结构](#目录结构)
- [启动流程](#启动流程)
- [中断与 APIC](#中断与-apic)
- [输入子系统](#输入子系统)
- [内存管理](#内存管理)
- [存储栈与 SRFS](#存储栈与-srfs)
- [图形与桌面](#图形与桌面)
- [字体链路](#字体链路)
- [应用](#应用)
- [Shell 命令](#shell-命令)
- [Ring 3 用户态](#ring-3-用户态)
- [登录与账户](#登录与账户)
- [工具与构建产物](#工具与构建产物)
- [调试](#调试)
- [已知问题与路线图](#已知问题与路线图)
- [故障排查](#故障排查)
- [致谢](#致谢)

---

## 功能总览

| 层次 | 已实现 |
| --- | --- |
| 引导 | Limine 协议；BIOS El Torito + UEFI `BOOTX64.EFI`；内核高半区运行；模块（logo / 字体）载入 |
| 内核基础 | GDT / TSS / IDT、异常与 IRQ、`SYSCALL`/`SYSRET`、CPUID、串口日志、CPUID 虚拟机检测 |
| 中断 | Local APIC（APIC_BASE + SVR + TPR）、IOAPIC 重定向（IRQ0→GSI2、IRQ1→GSI1、IRQ12→GSI12）、LAPIC EOI；失败自动回退 8259 |
| 内存 | 位图 PMM（4 KiB 页帧，最多 4 GiB）、4 级页表 VMM（增量 map/unmap、大页拆分、USER/NX）、First-Fit 内核堆 |
| 驱动 | 8250 串口（仅虚拟机）、PIC、PIT 100 Hz、PS/2 键盘 / 鼠标、PCI、AHCI(SATA, DMA)、RTC、ACPI 关机 / i8042 重启、xHCI USB HID 键盘鼠标（轮询） |
| 输入 | 统一输入子系统：256 项事件队列；PS/2 与 xHCI 只投递，GUI/登录只消费 |
| 存储 | MBR 分区解析、块设备抽象、VFS 最长前缀挂载、SRFS、磁盘工具（分区级快格） |
| 图形 | 帧缓冲、后台缓冲 + 脏区合成、窗口管理器、任务栏 / 开始菜单 / 右键菜单、SSFN 矢量字体、开机动画 |
| 应用 | 终端（Shell）、此电脑、磁盘工具、任务管理器、系统监视器、系统属性、全屏登录 |
| 用户态 | Ring 3 代码页 + 栈页、`sys_write` / `sys_exit`、越权访问异常隔离 |

**明确没有实现**：SMP、网络、音频、USB 存储、EHCI（USB 2.0 控制器）、FAT32、抢占调度、ELF 加载器、文件权限、中文输入法。

---

## 设计取舍

| 选择 | 原因 | 代价 |
| --- | --- | --- |
| 静态缓冲区优先 | 中断上下文不能分配；行为可预测 | BSS 较大（后台缓冲 ~8 MB），容量固定 |
| 轮询驱动（AHCI / xHCI） | 不依赖中断路由与共享状态，超时可控 | 占用主循环时间，高负载空转 |
| 单核事件循环 | 避免锁、调度器与上下文切换 | 无法并行；长任务必须投递到主循环 |
| 前后台缓冲 + 脏区合成 | 避免撕裂，减少 MMIO 写 | 需要维护脏矩形与绘制顺序 |
| 统一输入队列 | 驱动与 GUI 解耦；中断里只入队 | 多一次内存拷贝，事件延迟到下一轮主循环 |
| 自定义 SRFS | 布局简单，可用 Python 直接生成镜像 | 无日志、无权限、崩溃一致性弱 |
| 字体走 Limine module | Live CD / 自建空盘也能有中文 | 每次启动多占 5.3 MB 内存 |
| APIC 失败自动回退 8259 | 老平台/虚拟机都能起来 | 两条中断路径都要维护 |

---

## 快速开始

### 环境

推荐 **Linux / WSL2（Ubuntu 22.04+）**。Makefile 使用 `find`、`rm`、`cp`、`xorriso`，原生 Windows 无法直接 `make`。

```bash
sudo apt update
sudo apt install build-essential nasm python3 xorriso qemu-system-x86
```

- `gcc` + `ld`：x86_64 ELF（freestanding 参数见 `config.mk`）
- `nasm`：汇编 `.asm` 与 Ring 3 示例
- `python3`：生成 logo 头文件、用户态数组、磁盘镜像（仅标准库，无 PIL 依赖）
- `xorriso`：打包 ISO
- Limine 已预编译在 `limine/`，无需额外下载

### 构建

```bash
make            # build/StarRingOS.iso + build/disk.img
make run        # 构建并以 QEMU 启动（disk 目标会重建干净数据盘）
make disk       # 只重建 build/disk.img（会清空盘上数据）
make clean      # 删除 build/
```

> `make run` 依赖 `disk` 目标，每次都重生成数据盘。想保留 QEMU 里写入的文件，直接跑下面的 qemu 命令，别用 `make run`。

Makefile 自动发现 `kernel/**/*.c` 与 `*.asm`，**新增源文件无需登记**；`-MMD -MP` 生成头文件依赖，改 `.h` 会触发相关重编。

Windows 下用 WSL：

```powershell
wsl -d Ubuntu -- bash -lc "cd '/mnt/e/path/to/StarRing OS' && make"
```

### 构建需要哪些文件

| 类别 | 文件 |
| --- | --- |
| 构建入口 | `Makefile`、`config.mk`、`linker.lds`、`limine.conf` |
| 工具链 | `gcc`、`ld`、`nasm`、`python3`、`xorriso`、`make` + coreutils |
| 内核源码 | `kernel/` 全部 `.c` / `.asm` / `.h` |
| 字体 | `kernel/gfx/fontdata/siyuan.sfn`（5.3 MB，**缺失时 make 直接报错**） |
| Logo | `resource/logo.raw`（1 MB） |
| 引导 | `limine/` 下 6 个文件（含 `limine.h`、`BOOTX64.EFI`、`limine` 可执行） |
| 脚本 | `tools/mksrfs.py`、`gen_logo_header.py`、`gen_userbin.py` |
| 用户态 | `user/hello.asm`、`user/probe.asm` |

### 产物

| 文件 | 说明 |
| --- | --- |
| `build/StarRingOS.elf` | 内核 ELF（含 ~8 MB 后台缓冲 BSS） |
| `build/StarRingOS.iso` | 可引导 ISO（BIOS + UEFI，含 logo 与字体 module） |
| `build/disk.img` | 64 MB 示例盘：MBR + 一个 SRFS 分区，预置 `/usr/share/fonts/siyuan.sfn` |

---

## 运行

**QEMU（推荐，能看串口日志）**

```bash
make run
```

等价于：

```bash
qemu-system-x86_64 -cdrom build/StarRingOS.iso \
    -device ich9-ahci,id=sata \
    -drive id=disk0,file=build/disk.img,format=raw,if=none \
    -device ide-hd,drive=disk0,bus=sata.0 \
    -boot d -m 512M -serial stdio
```

无图形环境：

```bash
qemu-system-x86_64 -cdrom build/StarRingOS.iso \
    -drive if=none,id=disk0,file=build/disk.img,format=raw \
    -device ich9-ahci,id=sata -device ide-hd,drive=disk0,bus=sata.0 \
    -boot d -m 512M -display none -serial stdio -no-reboot
```

**VMware / VirtualBox / 实体机**

1. 新建 x86_64 虚拟机，内存 ≥ 512 MB，BIOS 或 UEFI 均可；
2. 挂载 `build/StarRingOS.iso`；
3. 再挂一块 **SATA** 硬盘（≥ 64 MB）。整块盘全零时内核会写 MBR + 0x53 类型 SRFS 分区（从 LBA 2048 开始）；
4. 已有分区但未格式化：进桌面后打开「磁盘工具」格式化；
5. 没有数据盘也能启动（只读访客模式），此时字体直接从 Limine module 使用，不落盘。

> 实体机建议 Ventoy / dd 写 U 盘。串口只在虚拟机里启用（见[调试](#调试)）：物理机没有 COM1 时不探测、不等待，日志静默丢弃。

---

## 目录结构

```text
StarRing OS/
├── Makefile / config.mk / linker.lds / limine.conf
├── limine/                    # 预编译引导器 + limine.h
├── resource/                  # logo.png / logo.raw
├── tools/                     # mksrfs.py / gen_logo_header.py / gen_userbin.py
├── user/                      # hello.asm / probe.asm（Ring 3）
├── kernel/
│   ├── main.c                 # kmain：完整启动顺序 + step_mark
│   ├── boot/                  # Limine 请求（全项目唯一一份）+ 开机 RTC/壁纸缓存
│   ├── arch/x86_64/           # GDT/TSS/IDT/ISR/SYSCALL/CPUID/hypervisor 检测
│   ├── mm/                    # PMM / VMM / heap / apic（LAPIC+IOAPIC）
│   ├── drivers/               # 串口/PIC/PIT/键盘/鼠标/RTC/PCI/AHCI/块设备/输入队列
│   │   ├── input.c            # 统一输入事件队列
│   │   └── usb/xhci.c         # xHCI Boot HID 键盘/鼠标（轮询）
│   ├── fs/                    # SRFS / VFS / MBR / .srs 配置
│   ├── gfx/                   # 帧缓冲、gfx、GUI、console、字体、开机动画
│   │   └── fontdata/          # 8x16 点阵、SSFN 头、siyuan.sfn、logo_data.h
│   ├── apps/                  # 终端/此电脑/登录/磁盘工具/任务管理器/系统监视器/系统属性
│   ├── shell/                 # Shell 解析与执行
│   ├── task/                  # 任务登记表（非抢占调度器）
│   ├── user/                  # Ring 3 运行器 + 内嵌用户程序数组
│   ├── hw/                    # CPU / 内存信息汇总
│   ├── lib/                   # 字符串、内存拷贝、busy_wait
│   └── include/               # 基础类型
└── build/                     # make 输出（不纳入版本管理）
```

---

## 启动流程

`kmain()` 的顺序（括号里是屏幕左上角的红色步骤号，见[调试](#调试)）：

```text
Limine → kmain
  ├─ serial_init()           串口（仅虚拟机；物理机直接禁用）
  ├─ fb_init() / gfx_init()  帧缓冲校验 + 后台缓冲
  ├─ tss_init() / idt_init() 异常处理先于动画
  ├─ splash_show()                                   [01]
  ├─ cpuinfo_init()                                  [02]
  ├─ meminfo_init() / pmm_init()                     [03]
  ├─ vmm_init()                                      [04]
  ├─ heap_init()                                     [05]
  ├─ task_init() / task_add("kernel")                [06]
  ├─ syscall_init()                                  [07]
  ├─ input_init()                                    （输入队列）
  ├─ apic_init() + ioapic_init() → 成功则 pic_disable()
  ├─ pic_remap()                                     [08]  （APIC 失败则回退 8259）
  ├─ keyboard_init()                                 [09]
  ├─ timer_init()                                    [10]
  ├─ mouse_init()                                    [11]
  ├─ xhci_kbd_init() / xhci_mouse_present()          [12]
  ├─ console_init()                                  [13]
  ├─ partition_scan() (+ 空白盘自动建 MBR)            [14]
  ├─ fs_mount_all()                                  [15]
  ├─ fs_write_selftest()                             [16]
  ├─ font_provision() + ttf_init_default()/module    [17]
  ├─ gui_init()                                      [18]
  ├─ login_run()                                     [19]
  └─ terminal_prompt() → sti → gui_run()（不返回）
```

设计要点：

- **异常处理先于动画**：GDT/TSS/IDT 在 `splash_show()` 之前装好，帧缓冲映射异常也能走 `#PF` 而不是三重故障。
- **开机动画不依赖磁盘**：优先用 Limine module 的 `resource/logo.raw`，否则用内核内置图；全程无磁盘 I/O。
- **登录是阻塞的**：`login_run()` 内部临时 `sti`，让 PIT / 键盘 / 鼠标可用；登录后直接进桌面，**不再自动弹终端、不再打印 banner**。
- **GUI 主循环**：`gui_run()` 在开中断的线程上下文运行，负责消费输入队列、执行投递动作、脏区合成。

---

## 中断与 APIC

老平台（HM55 一代、Legacy BIOS）常把 ISA 中断接到 IOAPIC，此时 8259 的 INTR 不再直连 CPU。中断链路按下面顺序建立：

1. **Local APIC**（`mm/apic.c`）
   - `IA32_APIC_BASE(0x1B)` bit11=1（硬件使能）；
   - `SVR(0xF0)` bit8=1 + 伪中断向量（软件使能）；
   - `TPR(0x80)` 清零，避免固件留下的门限压住中断。
2. **IOAPIC**（`0xFEC00000`）
   - 先屏蔽固件留下的全部重定向项；
   - 按 MP/ACPI 标准映射编程：`IRQ0→GSI2`（PIT，向量 32）、`IRQ1→GSI1`（键盘，33）、`IRQ12→GSI12`（鼠标，44）；
   - Fixed 投递、物理目的地、高电平有效、**边沿触发**（ISA 信号本身就是边沿）。
3. **8259 屏蔽**：IOAPIC 接管后整片 mask，并让 `pic_clear_mask()` 变空操作，避免同一条 IRQ 经 LINT0 ExtINT 与 IOAPIC 双投递。
4. **EOI 必须写 LAPIC**：`pic_send_eoi()` 在 APIC 模式下写 `0xFEE000B0`；只 EOI 8259 会留下 ISR 位，第二次同级中断被永久压住。

任何一步失败（映射失败、IOAPIC 版本异常），`apic_disable()` 会回到"8259 直连 CPU"的基线路径，保证 QEMU 等环境不回归。

相关文件：`kernel/mm/apic.c`、`kernel/drivers/pic.c`、`kernel/arch/x86_64/idt.c`。

---

## 输入子系统

```text
PS/2 键盘 ──IRQ1──┐
PS/2 鼠标 ──IRQ12─┤
xHCI HID  ──轮询──┴─► input_post_key()/input_post_mouse()
                              │
                   256 项事件队列（短临界区）
                              │
      gui_run() / login_run() ┴─► input_pump() + input_poll()
                                   ├─ INPUT_EV_KEY   → gui_key_event()
                                   └─ INPUT_EV_MOUSE → gui_mouse_event()
```

- 驱动层**只投递**，不再直接调用 `gui_mouse_event()`；GUI / 登录层**只消费**，不关心设备来自哪个控制器。
- PS/2 在中断上下文入队，xHCI 在主循环 `input_pump()` 里轮询后入队，两条生产者共用同一把短临界区。
- 鼠标事件不再在 IRQ12 里触发合成：拖动时只更新坐标与脏区，重绘交给主循环（详见[图形与桌面](#图形与桌面)）。

文件：`kernel/drivers/input.{c,h}`、`kernel/drivers/keyboard.c`、`kernel/drivers/mouse.c`、`kernel/drivers/usb/xhci.c`。

---

## 内存管理

| 模块 | 位置 | 说明 |
| --- | --- | --- |
| PMM | `kernel/mm/pmm.c` | 位图管理 Limine 报告的 USABLE 物理内存；4 KiB 页帧；静态位图 128 KiB，最多 4 GiB；保留前 1 MiB 与 0 号页 |
| VMM | `kernel/mm/vmm.c` | 在当前 PML4 上增量 map/unmap/translate；中间层按需分配；支持大页拆分、NX、USER |
| heap | `kernel/mm/heap.c` | 隐式空闲链表，16 字节对齐，First-Fit + Split + Coalesce；按需扩展，上限 128 MiB |
| 后台缓冲 | `kernel/gfx/gfx.c` | `static uint32_t backbuffer[1920*1080]`（~8 MB BSS），逻辑分辨率可小于真实帧缓冲，上屏时缩放 |

> `kmalloc` 目前主要给 SSFN 字形缓存与字体 blob 使用；GUI 应用都是静态单例。

**HHDM 请求只允许一份**：所有 Limine 请求集中在 `kernel/boot/limine_requests.c`，其它模块通过 `boot/limine_requests.h` 的 `extern` 使用。重复 request ID 会让 Limine 在加载内核前 `PANIC: Conflict detected for request ID ...`（现象：串口零输出、停在 Limine 错误页）。

---

## 存储栈与 SRFS

```text
应用（Shell / 此电脑 / 磁盘工具）
        │ fs_* / vfs_*
        ▼
VFS 最长前缀路由 ──► "/" → SRFS（根卷）；"/mnt/disk0p2" → SRFS（其它分区）
        ▼
SRFS（超级块、inode、目录项、块缓存、快格）
        ▼
block_device（分区视图，LBA 相对分区起点）
        ▼
AHCI（轮询 + DMA、PRDT、FLUSH CACHE EXT、超时/重试/端口恢复）
        ▼
SATA 磁盘
```

### 块设备与分区

- MBR 解析在 `kernel/fs/partition.c`：每盘最多 4 个主分区，最多 32 块物理盘；
- 分区类型：`0x53` = SRFS（优先），`0x83` = Linux（也按 SRFS 尝试挂载）；
- 新分区默认从 **LBA 2048（1 MiB 对齐）** 开始；
- `kernel/drivers/blkdev.c` 把分区登记为 `disk0p1`…，文件系统只看到分区内相对 LBA；
- 只有 `disk0` 完全空白时才自动创建 MBR，避免误初始化用户数据盘。

### SRFS 布局

块大小 **4096 字节**，块号均为分区内相对块号。

| 块号 | 内容 |
| --- | --- |
| 0 | 引导保留 |
| 1 | 超级块（108 字节，magic `SRFS` / `0x53465253`，含 64 字节卷标） |
| 2 | 块位图 |
| 3 | inode 位图 |
| 4 – 19 | inode 表（1024 × 64 字节） |
| 20 – | 数据区（块 20 为根目录） |

inode 结构（64 字节）：`mode(2) + nlink(2) + size(4) + 10 个直接块(40) + 间接块(4) + mtime(4) + 二级间接块(4) + pad(4)`。
目录项为 `inode(4) + name(60)`，每个目录至少含 `.` 与 `..`。

### 默认目录树

`fs_bootstrap()` 幂等补齐：

```text
/bin  /boot  /dev  /etc  /home/guest/{Desktop,Documents,Downloads,Pictures,Music}
/mnt  /tmp  /usr/bin  /usr/share/fonts  /var/log
```

并写入 `/readme.txt`、`/etc/motd`、`/etc/version`、`/etc/system.srs`（`font_path=/usr/share/fonts/siyuan.sfn`）、`/home/guest/*` 与 `/bin/*` 占位文件。

### 格式化与自检

- **快格**：只重建超级块、位图、inode 表与根目录，不擦除整个数据区；
- `fs_format_disk_ex(start_lba, cb, user)` 只格式化选中的 MBR 分区，绝不覆盖 MBR；
- 每次启动 `fs_write_selftest()` 写 `/hello.txt` → 读回逐字节比对 → 确认目录项存在，失败会在串口给出具体原因。

---

## 图形与桌面

### 渲染管线

- 真实帧缓冲来自 Limine；`fb_init()` 校验地址/宽高/bpp/pitch，拒绝非法几何；
- 应用只写 backbuffer，`composite()` 把脏矩形缩放拷贝到真实帧缓冲；
- 逻辑分辨率上限 1920×1080；当前为排查真机问题**临时强制 1024×768**（`gfx.c` 的 `GFX_FORCE_W/H`）。

### 局部重绘与拖动优化

- 每个绘制元素先做矩形粗筛（`rect_hits_clip`）：屏幕外的窗口/图标/任务栏整块跳过，省掉 TTF 字形查找与逐像素混合；
- 拖动中的窗口：不画阴影、标题栏用纯色、跳过内容区 `w->draw()`，松手时补一帧完整内容；
- **拖动不在中断里合成**：IRQ12 只更新坐标与脏区，真正的合成由 `gui_run()` 主循环完成（避免 i8042 丢字节导致光标乱跳）；
- 纯鼠标移动走指针快速路径，只重画指针矩形。

### 交互

- 桌面图标：此电脑、终端、磁盘工具、任务管理器、系统监视器；右键「打开 / 属性」（属性目前只对"此电脑"有效）；
- 开始菜单：左下角「开始」，含全部应用 + 个人文件夹 + 重启 / 关机；
- 任务栏：窗口按钮 + 时钟（RTC 时间，失败回退开机时长）；
- 窗口：拖动标题栏移动、最大化/还原、关闭；双击标题栏最大化；右键标题栏「最大化/还原 / 关闭」；
- 右键菜单：桌面空白（刷新 / 打开终端 / 此电脑）、图标（打开 / 属性）、"此电脑"内（打开 / 复制 / 剪切 / 粘贴 / 重命名 / 删除 / 属性 等）。

---

## 字体链路

```text
siyuan.sfn（5.3 MB）
  ├─ Limine module（limine.conf）──────────────┐
  ├─ ISO: /usr/share/fonts/siyuan.sfn          │
  └─ 盘上：/usr/share/fonts/siyuan.sfn          │
                                                ▼
kmain[17]: font_provision()  → 盘上没有且 module 有：写入 SRFS（幂等，尺寸不符会重写）
        ttf_init_default()   → 读 /etc/system.srs 的 font_path
        ttf_init_module()    → 无盘 / 落盘失败时直接吃 module 内存（零拷贝）
                                                ▼
SSFN 引擎（16 px）：ssfn_bbox + ssfn_render 烘成字形单元 → 512 槽 LRU 缓存
                                                ▼
ttf_draw_text()：按 UTF-8 码点逐字绘制，/255 alpha 混合，越界裁剪
```

- `gui_text` / `gui_text_max` / `gui_str_width` 全部走 TTF；量宽与绘制同源，中文排版不错位；
- 缺字时画空心方框占位，绝不静默留白；
- 字体加载失败时回退 8×16 点阵（只覆盖 ASCII）；
- 开机串口会打印字体指纹与自检：`font: ok ctx.size= line= hash=`、`font probe U+6B64 box=.. ink=N`、`face fam=.. cjk=1`，用于判断"字体缺 CJK"还是"渲染问题"。

文件：`kernel/gfx/font_ttf.c`、`kernel/gfx/font.c`、`kernel/gfx/fontdata/ssfn.h`。

---

## 应用

| 应用 | 打开方式 | 主要功能 |
| --- | --- | --- |
| **终端** | 桌面图标 / 开始菜单 / 右键 / `files` 内"在此打开终端" | Shell；命令历史、行编辑（←/→/Home/End、Backspace/Delete） |
| **此电脑** | 桌面图标 / 开始菜单 / `files [路径]` | 左侧导航 + 面包屑 + 大图标网格；复制/剪切/粘贴、重命名、删除、属性、新建文件/文件夹；Ctrl 多选、框选、拖放；驱动器容量条 |
| **磁盘工具** | 桌面图标 / `format` | 物理盘信息 + MBR 分区列表；选择分区；二次确认；真实写盘进度；保护活动系统分区 |
| **任务管理器** | 桌面图标 / `taskmgr` | 任务/窗口列表、PID、运行时间、处理器占用曲线、内存/磁盘 I/O 概要；结束任务 |
| **系统监视器** | 桌面图标 / `sysmon` | PMM 页帧统计、VMM 映射/页表统计、一键分页自检 |
| **系统属性** | 桌面「此电脑」右键 → 属性 | 版本、处理器型号、核心数、内存、内核名 |
| **登录** | 开机自动 | 锁屏时钟 → 密码页；默认账户 `guest`（空密码）；多用户（最多 8 个）；无盘时只读访客模式 |

---

## Shell 命令

```text
help  ls [路径]  cat <文件>  stat <路径>  mkdir <目录>  touch <文件>
cp <源> <目标>  rm <路径>  cd <路径>  pwd  echo [文本] > 文件
df  lsblk  mem  ptest  tasks  taskmgr  sysmon  files [路径]  format
ring3  ring3probe  clear
```

输出全中文；错误信息来自 `fs_strerror()`（如「文件或目录不存在」）。表格（`lsblk`/`tasks`）按"显示格"对齐，CJK 记 2 格。

---

## Ring 3 用户态

- 进入：`asm_enter_user(entry, ustack)`；返回内核用 `iretq`（GDT 中用户段顺序与 `sysretq` 的硬编码相反）。
- 地址窗口：`0x400000 – 0x402000`（代码页 + 栈页），内核校验用户指针。
- 系统调用（Linux 风格）：`rax`=调用号，`rdi rsi rdx r10 r8 r9`=参数，返回 `rax`。

| 调用号 | 名称 | 说明 |
| --- | --- | --- |
| 1 | `SYS_WRITE` | 写终端 / 串口 |
| 60 | `SYS_EXIT` | 退出并返回内核 |

- 用户程序用 NASM 写成扁平二进制（`user/*.asm`），构建时转成 C 数组内嵌，运行时拷到代码页执行；
- **异常隔离**：`probe.asm` 故意读内核地址 `0xffffffff80000000`，内核页表已清 USER 位，触发 `#PF` 后安全退回内核并继续启动，而不是整机挂死。
- 开机不再自动运行这两个演示；在 Shell 里执行 `ring3` / `ring3probe` 手动验证。

---

## 登录与账户

- 用户数据库：`/etc/users.dat`（magic `LOGI`，版本 1）；
- 密码：FNV-1a 32 位哈希（仅演示，**不是安全边界**）；
- 默认账户 `guest`，空密码（登录时直接回车）；
- 密码页可切到用户选择器，并通过 "+ New user" 创建账户，信息写回 SRFS；
- 无可用文件系统时 `guest_mode = true`，显示「只读访客模式（无文件系统）」，不允许创建用户。

---

## 工具与构建产物

| 工具 | 作用 |
| --- | --- |
| `tools/mksrfs.py` | 生成 `disk.img`：LBA 0 MBR + 0x53 分区（LBA 2048 起）+ SRFS 元数据 + 字体 |
| `tools/gen_logo_header.py` | `resource/logo.raw` → `kernel/gfx/fontdata/logo_data.h` |
| `tools/gen_userbin.py` | `build/hello.bin` / `probe.bin` → 内核可 `#include` 的 C 数组（单文件 ≤ 4 KiB） |
| `gen_raw.py`（根目录） | 开发辅助：PNG → 512×512 `logo.raw`（需要 PIL，日常构建不需要） |
| `ssfn_2.0.0-amd64.deb` | SSFN 工具包：`sfnconv` 把 OTF/TTF 转成 `.sfn`，日常构建不需要 |

手动重建磁盘镜像：

```bash
python3 tools/mksrfs.py build/disk.img resource/logo.raw kernel/gfx/fontdata/siyuan.sfn 64
```

---

## 调试

### 屏幕步骤标记（物理机无串口时的主要手段）

`main.c` 里保留两个调试函数：

- `step_mark(n)`：屏幕左上角（y=2）红色两位数，表示**已完成**到第 n 步（对应[启动流程](#启动流程)编号 01–19）；
- `dev_mark(y, s, col)`：y=40/58/76/94 的设备状态行，黄 = 中性，绿 = 成功，红 = 失败/不存在；会被 `gui_init()` 首帧与登录页重绘覆盖，**只能在启动过程中观察**。

### 串口日志

- COM1，115200 8N1，所有内核日志以 `[SRCore]` 开头；
- **只在虚拟机里初始化**（CPUID hypervisor 位）：物理机没有 COM1 时不探测、不等待，避免被假串口拖死；
- QEMU 用 `-serial stdio` 或 `-serial file:...` 收集。

### 内置自检

| 自检 | 触发 | 验证内容 |
| --- | --- | --- |
| SRFS 写盘自检 | 每次启动 | 写 `/hello.txt` → 读回比对 → 确认目录项 |
| 分页自检 | `ptest` / 系统监视器按钮 | PMM 分配 → VMM 映射 → 写读 → 解除映射 → 释放 |
| 字体自检 | 每次启动（串口） | 分辨率、字体指纹、ASCII/汉字真实墨水像素、字体片段是否含 CJK |
| Ring 3 Hello | `ring3` | 用户态 `sys_write` + `sys_exit` |
| Ring 3 越权探针 | `ring3probe` | 读内核地址触发 `#PF` 并被安全回收 |

### 当前保留的调试痕迹

- `main.c` 的 `step_mark` / `dev_mark` 调用（定位完成后可整段删除）；
- `gfx.c` 强制 1024×768（`GFX_FORCE_W/H`）与 `limine.conf` 的 `1024x768x32`；
- 这些都不影响功能，只影响观感/分辨率。

---

## 已知问题与路线图

**架构限制**

- 单核、无 SMP；没有抢占调度器（`task/task.c` 只是任务登记表）。
- 桌面单线程事件循环：输入事件统一在 `gui_run()` 主循环里消费，`on_click` / `on_hover` / `on_key` 都在主循环上下文执行；耗时工作仍应 `gui_post_action()` 投递，避免卡住合成。
- 没有网络、音频、USB 存储、EHCI（USB 2.0）、FAT32、ELF 加载器、文件权限、中文输入法。
- 登录/密码只是演示，不是安全机制。
- 窗口只能最大化/还原，不能自由缩放。

**实体机（Acer Aspire / i3-380M / HM55 / Legacy BIOS）现状**

- 老平台没有 xHCI，USB 枚举会快速失败；输入只能靠 PS/2；
- `keyboard_init()` 在物理机跳过 i8042 编程（该机端口访问会被 SMI 拖到秒级，曾卡死启动），因此在固件未启用 IRQ1 时键盘可能无响应；
- `mouse_init()` 对所有平台都先做 `i8042_probe()`，探测不过只注册 IRQ12；
- 中断链路已改为 APIC + IOAPIC，并在失败时回退 8259；真机上的"时钟是否走"用登录页时钟判断。

**可能的下一步**

1. 实体机中断/输入收尾：验证 APIC+IOAPIC 后时钟与键盘；必要时解析 ACPI MADT 取真实 IOAPIC 基址；
2. 物理机上补最小 i8042 使能（只发 `KBD_ON` / 必要时写命令字节）而不阻塞启动；
3. EHCI（USB 2.0）驱动，覆盖只有 USB 2.0 的老机器；
4. APIC Timer 取代 PIT；SMP / AP 启动；
5. 网络栈（e1000 / virtio-net）、ELF 用户进程与更多 syscall；
6. 文件权限、日志式 SRFS、窗口缩放与 Alt+Tab；
7. 中文输入法（拼音）与更完整的 CJK 字体子集。

---

## 故障排查

| 现象 | 可能原因 / 处理 |
| --- | --- |
| 串口零输出，停在 Limine | 重复 request ID（`PANIC: Conflict detected for request ID`）；确认请求只在 `boot/limine_requests.c` 定义 |
| 有 Limine 菜单但进不去 | `limine.conf` 的 `path:` 与 ISO 内文件不一致；`resolution:` 必须在 entry 内缩进 |
| 开机后黑屏 / 花屏 | 帧缓冲几何或 bpp 不支持；看串口 `gfx ok, fb=...` |
| 物理机没有串口日志 | 预期行为：非虚拟机不初始化 COM1 |
| 物理机卡在某个 step_mark 数字 | 该数字之后的那一步异常；对照启动流程表 |
| 物理机时钟不走 | 中断路由问题：物理机无串口，看 step_mark 是否走完、登录页时钟是否跳；虚拟机里看 `irq: ioapic mode` / `pic mode (fallback)`，fallback 说明 IOAPIC 映射失败 |
| 中文显示为空心方框 | 字体缺对应码点；串口 `font probe ... ink=0` / `face ... cjk=0` 可确认，需要重转 `.sfn` |
| 中文整段不显示 | 字体没加载：串口 `font: REJECT (...)`；或 `ttf unavailable` |
| 文件写入成功但重启丢失 | 检查 `fs_sync()` / `FLUSH CACHE EXT` 日志；QEMU 需正常关机 |
| 提示 no filesystem | 盘上没有 SRFS 分区：空白 disk0 会自动建 MBR；否则用磁盘工具格式化 |
| QEMU 启动很慢 | 无 KVM 时是 TCG 软件模拟，正常 |
| 鼠标乱跳 | 中断延迟导致包错位；已加断包超时（2 tick）与 9 位补码还原，仍异常可放宽阈值 |
| 4K 屏画面模糊 | 逻辑分辨率被强制 1024×768，上屏缩放导致；改回需调 `GFX_FORCE_W/H` 与 `limine.conf` |

---

## 致谢

- [Limine](https://github.com/limine-bootloader/limine)：现代 x86_64 引导器与协议。
- [SSFN（Scalable Screen Font）](https://gitlab.com/bztsrc/scalable-font2)：矢量字体渲染（`kernel/gfx/fontdata/ssfn.h` 与 `siyuan.sfn`）。
- QEMU / VMware 提供的虚拟硬件环境。

> 仓库目前未附带 `LICENSE`；如需开源发布，请先补充许可证。

---

**StarRing OS / SRCore** —— 从引导扇区到桌面，全部自己写。
