# StarRing OS

StarRing OS 是一个从零实现的 **x86_64 爱好型操作系统**：自研内核 **SRCore**、自研磁盘文件系统 **SRFS**、以及一套可交互的图形桌面。项目不依赖 libc / POSIX，内核与大部分应用以静态内存和轮询驱动为主，目标是"能在一台普通 PC（或 QEMU / VMware）上完整跑起来"。

- 当前版本：`0.1.0`（见 `/etc/version`）
- 引导方式：Limine（BIOS + UEFI 双支持），内核加载到高半区 `0xffffffff80000000`
- 开发语言：C（freestanding）+ NASM
- 运行形态：单核、单线程事件循环 + 中断；无抢占式调度器

> 这是一个教学 / 实验性质的项目：代码里保留了大量中文注释，用来解释"为什么这么做"，而不仅是"做了什么"。

---

## 目录

- [功能总览](#功能总览)
- [设计取舍](#设计取舍)
- [快速开始](#快速开始)
- [目录结构](#目录结构)
- [启动流程](#启动流程)
- [内存管理](#内存管理)
- [存储栈](#存储栈)
- [SRFS 文件系统](#srfs-文件系统)
- [图形与桌面](#图形与桌面)
- [Shell 命令](#shell-命令)
- [Ring 3 用户态](#ring-3-用户态)
- [工具与构建产物](#工具与构建产物)
- [调试与当前状态](#调试与当前状态)
- [已知限制与路线图](#已知限制与路线图)
- [故障排查](#故障排查)
- [致谢](#致谢)

---

## 功能总览

| 层次 | 已实现 |
| --- | --- |
| 引导 | Limine 协议；BIOS El Torito + UEFI `BOOTX64.EFI`；内核高半区运行 |
| 内核基础 | GDT / TSS / IDT、异常与 IRQ、`SYSCALL`/`SYSRET`、CPUID、串口日志 |
| 内存 | 位图 PMM（4 KiB 页帧，最大 4 GiB）、4 级页表 VMM（基于 Limine 页表增量映射）、First-Fit 内核堆 |
| 驱动 | 8250 串口、PIC、PIT(100 Hz)、PS/2 键盘、PS/2 鼠标、xHCI USB 键盘、PCI、AHCI(SATA, DMA)、RTC(CMOS)、ACPI 关机 / i8042 重启 |
| 存储 | MBR 分区解析、块设备抽象、VFS 最长前缀挂载路由、SRFS、磁盘工具（分区级快格） |
| 图形 | 帧缓冲驱动、后台缓冲 + 脏区合成、窗口管理器、任务栏 / 开始菜单 / 右键菜单、SSFN 矢量字体 + 8×16 点阵兜底、开机动画 |
| 应用 | 终端（Shell）、文件管理器（This PC）、磁盘工具、任务管理器、系统监视器、全屏登录 |
| 用户态 | Ring 3 代码页 + 栈页、`sys_write` / `sys_exit`、越权访问异常隔离 |
| 工具 | Python 磁盘镜像生成、Logo 头文件生成、用户程序二进制内嵌 |

**明确没有实现**：SMP、网络、音频、USB 存储、FAT32 驱动、抢占式调度、完整的用户进程加载器（ELF）、文件权限。详见[已知限制](#已知限制与路线图)。

---

## 设计取舍

| 选择 | 原因 | 代价 |
| --- | --- | --- |
| 静态缓冲区优先 | 早期没有堆；中断上下文不能分配；行为可预测 | BSS 较大（后台缓冲约 8 MB），容量固定 |
| 轮询驱动（AHCI / xHCI） | 不依赖中断路由与共享状态，超时可控 | 占用主循环时间，高负载下 CPU 空转 |
| 单核事件循环 | 避免锁、调度器与上下文切换的复杂度 | 无法并行；长任务必须投递到主循环 |
| 前后台缓冲 + 脏区合成 | 避免撕裂，减少对 MMIO 帧缓冲的写入 | 需要维护脏矩形与合成顺序 |
| 自定义 SRFS | 布局简单，可在 Python 里直接生成镜像 | 无日志、无权限、崩溃一致性弱 |
| Limine 高半区内核 | 直接获得分页、HHDM、帧缓冲与模块加载 | 依赖 Limine 协议；请求 ID 必须全局唯一 |
| 串口作为第一调试界面 | 无 GUI / 无屏幕也能定位早期故障 | 真机 COM1 常被 BIOS 关闭，需要额外硬件 |

---

## 快速开始

### 1. 环境要求

推荐在 **Linux / WSL2（Ubuntu 22.04+）** 下构建。Makefile 使用了 `find`、`rm`、`xorriso` 等 Unix 工具，原生 Windows 环境无法直接 `make`。

```bash
sudo apt update
sudo apt install build-essential nasm python3 xorriso qemu-system-x86
```

- 交叉/本地编译器：`gcc` + `ld`（生成 x86_64 ELF 即可）
- `nasm`：汇编 `*.asm` 与用户态示例
- `python3`：生成磁盘镜像与内嵌数组
- `xorriso`：打包 ISO
- `qemu-system-x86_64`：运行（可选）
- Limine 已预编译在 `limine/` 目录，无需额外下载

### 2. 构建

```bash
make            # 生成 build/StarRingOS.iso 和 build/disk.img
make run        # 构建并用 QEMU 启动（会通过 disk 目标重建干净的 64 MB 数据盘）
make disk       # 强制重建 build/disk.img（会清空盘上数据）
make clean      # 删除 build/
```

> `make run` 依赖伪目标 `disk`，因此每次都会重新生成数据盘；若想保留上次在 QEMU 里写入的文件，请直接执行下面的 `qemu-system-x86_64` 命令，而不要用 `make run`。

Makefile 会自动发现 `kernel/` 下的所有 `.c` / `.asm`（含头文件依赖跟踪），新增源文件不需要手动登记。

Windows 下通过 WSL 构建示例：

```powershell
wsl -d Ubuntu -- bash -lc "cd '/mnt/e/path/to/StarRing OS' && make"
```

构建产物：

| 文件 | 说明 |
| --- | --- |
| `build/StarRingOS.elf` | 内核 ELF（约 2–3 MB，含 8 MB 后台缓冲 BSS） |
| `build/StarRingOS.iso` | 可引导 ISO（BIOS + UEFI） |
| `build/disk.img` | 64 MB 示例磁盘：MBR + 一个 SRFS 分区，含 `/boot/logo.raw` |
| `build/iso_root/` | ISO 打包临时目录 |

### 3. 运行

**QEMU（推荐，便于看串口日志）**

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

无图形环境 / 自动化测试时：

```bash
qemu-system-x86_64 -cdrom build/StarRingOS.iso \
    -drive if=none,id=disk0,file=build/disk.img,format=raw \
    -device ich9-ahci,id=sata -device ide-hd,drive=disk0,bus=sata.0 \
    -boot d -m 512M -display none -serial stdio -no-reboot
```

**VMware / VirtualBox / 实体机**

1. 新建 x86_64 虚拟机，内存 ≥ 512 MB，固件 BIOS 或 UEFI 均可；
2. 挂载 `build/StarRingOS.iso` 为光盘；
3. 再添加一块 **SATA** 硬盘（≥ 64 MB）。首次启动时若整块盘全零，内核会自动写入 MBR + 一个 0x53 类型的 SRFS 分区（从 LBA 2048 开始）；
4. 若磁盘已有分区但未格式化，进入桌面后打开 **Disk Utility** 选择分区格式化；
5. 没有数据盘也能启动：系统会进入"只读访客模式"（无法保存登录账户与文件）。

> 实体机建议用 Ventoy / dd 写入 U 盘。串口日志走 COM1（115200 8N1），没有串口时日志会被静默丢弃，不影响启动。

---

## 目录结构

```text
StarRing OS/
├── Makefile                  # 构建入口：内核 / ISO / 磁盘镜像 / QEMU
├── config.mk                 # gcc / ld 编译选项
├── linker.lds                # 高半区内核链接脚本，ENTRY(kmain)
├── limine.conf               # Limine 启动项（分辨率、内核路径、logo module）
├── ssfn_2.0.0-amd64.deb      # SSFN 字体工具包（sfnconv / sfnedit，生成 font.sfn 用）
├── gen_raw.py                # 开发辅助：PNG → logo.raw
├── limine/                   # 预编译 Limine：BIOS 阶段、UEFI BOOTX64.EFI、limine.h
├── resource/
│   ├── logo.png              # 原始 Logo
│   └── logo.raw              # 512×512×32bpp ARGB，小端
├── user/
│   ├── hello.asm             # Ring 3 "Hello" 示例（sys_write + sys_exit）
│   └── probe.asm             # Ring 3 越权读内核地址探针
├── tools/
│   ├── mksrfs.py             # 生成 MBR + SRFS 磁盘镜像
│   ├── gen_logo_header.py    # logo.raw → kernel/gfx/fontdata/logo_data.h
│   └── gen_userbin.py        # 扁平二进制 → C 数组头文件
├── kernel/
│   ├── main.c                # kmain：完整启动顺序
│   ├── boot/                 # Limine 请求的唯一集中定义处
│   ├── arch/x86_64/          # GDT/TSS/IDT/ISR/SYSCALL/CPUID
│   ├── mm/                   # PMM / VMM / heap
│   ├── drivers/              # 串口/PIC/PIT/键盘/鼠标/RTC/PCI/AHCI/块设备/xHCI/电源
│   │   └── usb/xhci.c        # USB 3.x Boot HID 键盘（轮询）
│   ├── fs/                   # SRFS + VFS + MBR 分区
│   ├── gfx/                  # 帧缓冲、绘制、GUI、控制台、字体、开机动画
│   │   └── fontdata/         # 8×16 点阵、SSFN 头文件与 font.sfn
│   ├── apps/                 # 终端/文件管理器/登录/磁盘工具/任务管理器/系统监视器
│   ├── shell/                # Shell 命令解析与执行
│   ├── task/                 # 任务登记表（非抢占调度器）
│   ├── user/                 # Ring 3 运行器与内嵌用户程序数组
│   ├── hw/                   # CPU / 内存信息汇总
│   ├── lib/                  # 字符串、内存拷贝、busy_wait
│   └── include/              # 基础类型
└── build/                    # make 输出（不纳入版本管理）
```

---

## 启动流程

```text
Limine (BIOS / UEFI)
  └─ 解析 limine.conf，加载 StarRingOS.elf 到高半区
      └─ kmain()  [kernel/main.c]
          ├─ serial_init()                 串口日志
          ├─ fb_init() / gfx_init()        帧缓冲校验 + 后台缓冲
          ├─ tss_init() / idt_init()       在开机动画前装好异常处理
          ├─ splash_show()                 用 Limine module（或内嵌兜底）播放动画
          ├─ cpuinfo / meminfo
          ├─ pmm_init() → vmm_init() → heap_init()
          ├─ task_init() / syscall_init()
          ├─ pic_remap() / keyboard_init() / timer_init() / [mouse_init()]
          ├─ xhci_kbd_init()               可选 USB 键盘
          ├─ console_init()
          ├─ partition_scan()              扫描所有 AHCI 盘 MBR
          │    └─ 空白 disk0：自动创建 MBR + SRFS 分区
          ├─ fs_mount_all()                根卷 → "/"，其它卷 → /mnt/<bdev>
          │    └─ fs_write_selftest()      写 /hello.txt 并读回比对
          ├─ ttf_init("/usr/share/fonts/font.sfn")
          ├─ gui_init()                    桌面第一帧
          ├─ login_run()                   全屏锁屏/登录（阻塞，内部临时 sti）
          ├─ terminal_open() → show_banner()
          ├─ usermode_run_demo()           Ring 3 Hello
          ├─ usermode_run_probe()          Ring 3 越权隔离验证
          └─ sti → gui_run()               桌面事件主循环（不返回）
```

几个设计要点：

- **异常处理先于动画**：GDT/TSS/IDT 在 `splash_show()` 之前初始化。即使真机帧缓冲映射异常，也能走 `#PF` 处理而不是三重故障重启。
- **开机动画不依赖磁盘**：优先用 Limine 以 module 形式加载到内存的 `resource/logo.raw`；没有则用内核内置的 `logo_data.h`（`assets_logo_raw`）。这一步发生在挂载文件系统之前，不读写 SRFS。
- **登录是阻塞的**：`login_run()` 内部临时开中断，让 PIT / 键盘 / 鼠标可用；认证成功后通过 post-login hook 打印横幅。
- **GUI 主循环**：`gui_run()` 在开中断的线程上下文运行，负责合成、投递动作队列、轮询 xHCI 键盘。

---

## 内存管理

| 模块 | 位置 | 说明 |
| --- | --- | --- |
| PMM | `kernel/mm/pmm.c` | 位图管理 Limine 报告的 USABLE 物理内存；4 KiB 页帧；静态位图 128 KiB，最多 4 GiB；保留前 1 MiB 与 0 号页；alloc/free 带中断临界区 |
| VMM | `kernel/mm/vmm.c` | 在当前 PML4（Limine 已开启分页）上增量 map/unmap/translate；通过 HHDM 访问页表；中间层页表按需分配、不回收；支持 NX / USER 位 |
| heap | `kernel/mm/heap.c` | 隐式空闲链表，16 字节对齐，块头 32 字节；First-Fit + Split + Coalesce；通过 PMM+VMM 动态扩展；虚拟区间 `0xffffa00000000000`，上限 128 MiB |
| 后台缓冲 | `kernel/gfx/gfx.c` | `static uint32_t backbuffer[1920*1080]`（约 8 MB BSS），逻辑分辨率可小于真实帧缓冲，上屏时缩放 |

**HHDM 请求只允许有一份**。所有 Limine 请求集中定义在 `kernel/boot/limine_requests.c`，其它模块通过 `boot/limine_requests.h` 的 `extern` 使用。Limine 对重复的 request ID 会直接 `PANIC: Conflict detected for request ID ...`，内核根本不会开始执行。

> 目前 `kmalloc` 的主要使用者是 SSFN 矢量字体缓存（`font_ttf.c`）；GUI 应用仍然以静态单例为主。

---

## 存储栈

```text
应用（Shell / 文件管理器 / 磁盘工具）
        │  fs_* / vfs_*
        ▼
VFS 最长前缀路由  ──►  "/"        → SRFS（根卷）
        │                "/mnt/disk0p2" → SRFS（其它分区）
        ▼
SRFS（超级块、inode、目录项、块缓存、快格）
        ▼
block_device（分区视图：LBA 相对分区起点）
        ▼
AHCI（轮询 + DMA、PRDT、FLUSH CACHE EXT、超时/重试/端口恢复）
        ▼
SATA 磁盘
```

### 块设备与分区

- MBR 解析位于 `kernel/fs/partition.c`：每盘最多 4 个主分区，最多 32 块物理盘。
- 分区类型：`0x53` = SRFS（优先），`0x83` = Linux（也按 SRFS 尝试挂载）。
- 新分区默认从 **LBA 2048（1 MiB 对齐）** 开始。
- `kernel/drivers/blkdev.c` 把每个分区登记为 `disk0p1`、`disk0p2`… 的块设备，文件系统只看到分区内相对 LBA。
- 只有 `disk0` 完全空白时才会自动创建 MBR，避免误初始化用户的数据盘。

### AHCI 驱动

`kernel/drivers/ahci.c` 是一个**纯轮询、无中断、无 malloc** 的 SATA 驱动：

- 所有命令列表 / FIS / 命令表 / bounce buffer 都是全局静态数组；
- 通过 PCI 扫描 AHCI 控制器，并显式打开 `MEM_SPACE` 与 `BUS_MASTER`（VMware 可能默认关闭 BME，导致 DMA 永远不动）；
- 写命令会验证 Command Header 的 `W` 位与 FIS 的 `C` 位，`sfence` 后写 `PxCI`，轮询 `PxCI` 清零，再检查 `TFD/SERR`；
- 支持 `FLUSH CACHE EXT`，`fs_sync()` 只在确有脏块时下发；
- 失败时最多尝试 3 次（含首次），并用 `CLO` + 端口重启做恢复，所有等待都有上限。

---

## SRFS 文件系统

SRFS 是一个类 ext2 的简单文件系统，块大小 **4096 字节**，块号均为**分区内相对块号**。

| 块号 | 内容 |
| --- | --- |
| 0 | 引导保留 |
| 1 | 超级块（104 字节，magic `SRFS` / `0x53465253`） |
| 2 | 块位图 |
| 3 | inode 位图 |
| 4 – 19 | inode 表（1024 个 inode × 64 字节） |
| 20 – | 数据区（块 20 为根目录） |

inode 结构（64 字节）：

```text
mode(2) + nlink(2) + size(4) + 10 个直接块(40) + 1 个间接块(4) + 保留(12)
```

目录项为 `inode(4) + name(60)`，根目录与每个目录至少包含 `.` 和 `..`。

### 默认目录树

首次挂载或格式化后，`fs_bootstrap()` 会幂等补齐：

```text
/bin  /boot  /dev  /etc  /home/guest/{Desktop,Documents,Downloads,Pictures,Music}
/mnt  /tmp  /usr/bin  /var/log
```

并写入示例文件：`/readme.txt`、`/etc/motd`、`/etc/version`、`/home/guest/welcome.txt`、`/home/guest/Documents/notes.txt`、`/home/guest/Desktop/readme.txt`，以及 `/bin/*` 命令占位文件。

### 格式化

- **快格（quick format）**：只重建超级块、位图、inode 表与根目录，不擦除整个数据区。
- `fs_format_disk_ex(start_lba, cb, user)` 只格式化选中的 MBR 分区，**绝不传 0 覆盖 MBR**。
- 格式化非活动分区后，当前系统继续可用；格式化活动根分区后 `fs_ready()` 变为 `false`，必须重启，由 `fs_mount_all()` + `fs_bootstrap()` 重新建立目录树。
- Disk Utility 会禁用"格式化当前系统分区"按钮，并要求二次确认。

### 开机写盘自检

`main.c` 的 `fs_write_selftest()` 每次启动都会：

1. 写 `/hello.txt`（内容 `test`，4 字节）；
2. 读回并逐字节比较；
3. `fs_listdir("/")` 确认目录项已落盘。

失败会在串口打印具体原因，避免"写命令假成功、盘上其实没内容"被静默吞掉。

---

## 图形与桌面

### 分辨率与渲染

- 真实帧缓冲来自 Limine；`fb_init()` 会校验地址、宽高、bpp、pitch，拒绝非法几何。
- 内核使用**后台缓冲 + 脏区合成**：应用只写 backbuffer，`composite()` 负责把脏矩形缩放拷贝到真实帧缓冲。
- 逻辑分辨率上限 `1920×1080`；当前为排查真机问题**临时强制 1024×768**（见[调试与当前状态](#调试与当前状态)）。
- 字体：优先 SSFN 矢量字体 `/usr/share/fonts/font.sfn`，失败时回退 8×16 点阵。

### 桌面交互

- **桌面图标**：This PC、Terminal、Disk Utility、Task Manager、System Monitor。
- **开始菜单**：左下角 Start 按钮，包含上述应用 + Home folder + Restart / Shut down。
- **任务栏**：显示已打开窗口按钮与时钟（RTC 时间，读取失败时回退为开机时长）；点击任务栏按钮切换/聚焦窗口。
- **窗口**：拖动标题栏移动；标题栏右侧有"最大化/还原"和"关闭"两个按钮；双击标题栏也可最大化；右键标题栏有 Maximize / Restore / Close。
- **右键菜单**：桌面空白处（Refresh / Open Terminal / This PC）、桌面图标（Open）、文件管理器（Open / Copy / Cut / Paste / Rename / Delete / Properties 等）。
- **输入**：PS/2 键盘 + 可选 xHCI USB 键盘；PS/2 鼠标（当前被临时注释，见调试章节）。

### 应用一览

| 应用 | 打开方式 | 主要功能 |
| --- | --- | --- |
| **Terminal** | 桌面图标 / 开始菜单 / 右键 | 交互式 Shell；命令历史（↑/↓）、左右/Home/End 行编辑、Backspace/Delete |
| **This PC（文件管理器）** | 桌面图标 / `files` | 左侧导航 + 地址栏面包屑 + 大图标网格；复制/剪切/粘贴、重命名、删除、属性、新建文件夹/文件、Open Terminal Here；Ctrl 多选、框选、拖放（Ctrl 拖 = 复制）；驱动器容量条 |
| **Disk Utility** | 桌面图标 / `format` | 物理盘信息 + MBR 分区列表；选择分区；二次确认；真实写盘进度条；保护活动系统分区 |
| **Task Manager** | 桌面图标 / `taskmgr` | 任务/窗口列表、PID、运行时间、CPU/内存概要；结束任务 |
| **System Monitor** | 桌面图标 / `sysmon` | PMM 物理页帧统计、VMM 映射统计；一键分页自检（map → 写读 → unmap） |
| **Login** | 开机自动 | 锁屏时钟 → 密码页；默认账户 `guest`（空密码）；多用户（最多 8 个）；无文件系统时进入只读访客模式 |

### 登录与账户

- 用户数据库：`/etc/users.dat`（magic `LOGI`，版本 1）。
- 密码存储：FNV-1a 32 位哈希（仅用于演示，**不是安全边界**）。
- 默认账户：`guest`，密码为空（登录时直接回车即可）。
- 登录页可在密码页切到用户选择器，并通过 "+ New user" 创建账户；账户信息写回 SRFS。
- 没有可用文件系统时：`guest_mode = true`，登录页显示 "Read-only guest mode (no filesystem)"，不允许创建用户。

---

## Shell 命令

在 Terminal 窗口中输入（`help` 可查看）：

| 命令 | 说明 |
| --- | --- |
| `help` | 显示帮助 |
| `ls [path]` | 列目录 |
| `cat <file>` | 显示文件内容 |
| `stat <path>` | 显示 inode 信息 |
| `mkdir <dir>` | 创建目录 |
| `touch <file>` | 创建空文件 |
| `cp <src> <dst>` | 复制文件 |
| `rm <path>` | 删除文件或空目录 |
| `cd <path>` / `pwd` | 切换 / 显示当前目录 |
| `echo [text] > <file>` | 写入文本（仅支持 echo 重定向） |
| `df` | 磁盘块 / inode 使用率 |
| `lsblk` | MBR 分区表与块设备列表 |
| `mem` | 物理内存 / 分页统计 |
| `ptest` | 页表 map/unmap 自检 |
| `tasks` | 列出已登记任务 |
| `taskmgr` / `sysmon` | 打开任务管理器 / 系统监视器 |
| `files [path]` / `explorer` | 打开文件管理器 |
| `format` | 打开磁盘工具 |
| `ring3` | 运行 Ring 3 Hello 程序 |
| `ring3probe` | 验证 Ring 3 无法读取内核内存 |
| `clear` | 清屏 |

---

## Ring 3 用户态

- 进入方式：`asm_enter_user(entry, ustack)`；返回内核使用 `iretq`（不是 `sysretq`，因为本项目 GDT 中用户代码段/数据段的顺序与 `sysretq` 的硬编码相反）。
- 用户地址窗口：`0x400000 – 0x402000`（代码页 + 栈页），内核会校验用户指针。
- 系统调用约定（Linux 风格）：

```text
rax = 调用号
rdi, rsi, rdx, r10, r8, r9 = 参数 1..6
返回值 = rax
```

| 调用号 | 名称 | 说明 |
| --- | --- | --- |
| `1` | `SYS_WRITE` | 写终端 / 串口 |
| `60` | `SYS_EXIT` | 退出并返回内核 |

- 用户程序以 NASM 扁平二进制编写（`user/*.asm`），构建时转成 C 数组内嵌进内核；运行时拷贝到代码页执行。
- **异常隔离**：`probe.asm` 故意读取内核地址 `0xffffffff80000000`；由于内核页表已清除 USER 位，会触发 `#PF`，内核的兜底处理会安全退回内核并继续启动，而不是整机挂死。

---

## 工具与构建产物

| 工具 | 作用 |
| --- | --- |
| `tools/mksrfs.py` | 生成 `disk.img`：LBA 0 MBR + 0x53 分区（LBA 2048 起）+ SRFS 元数据 + `/boot/logo.raw` |
| `tools/gen_logo_header.py` | 把 `resource/logo.raw` 转成 `kernel/gfx/fontdata/logo_data.h`（开机动画兜底） |
| `tools/gen_userbin.py` | 把 `build/hello.bin` / `build/probe.bin` 转成内核可 `#include` 的 C 数组（单文件 ≤ 4 KiB） |
| `gen_raw.py`（根目录） | 开发辅助：用 PIL 把 `assets/logo.png` 缩放为 512×512 并输出 `logo.raw`（路径为旧版约定，日常构建不需要） |
| `ssfn_2.0.0-amd64.deb`（根目录） | SSFN 官方工具包：`sfnconv` 把 TTF 转成 `.sfn`，`sfnedit` 可视化编辑；`font.sfn` 由此生成，日常构建不需要 |

手动重建磁盘镜像：

```bash
python3 tools/mksrfs.py build/disk.img resource/logo.raw 64
```

---

## 调试与当前状态

### 串口日志

- COM1，115200 8N1；所有内核日志以 `[SRCore]` 开头。
- 真机 COM1 被 BIOS 关闭时，`serial_ready()` 为 false，输出静默丢弃，**不会**在等待 THRE 时卡死。
- QEMU 用 `-serial stdio` 或 `-serial file:...` 收集。

### 内置自检

| 自检 | 触发方式 | 验证内容 |
| --- | --- | --- |
| SRFS 写盘自检 | 每次启动自动 | 写 `/hello.txt` → 读回比对 → 确认目录项存在 |
| 分页自检 | `ptest` / System Monitor 按钮 | PMM 分配 → VMM 映射 → 写读 → 解除映射 → 释放 |
| Ring 3 Hello | 启动自动 / `ring3` | 用户态 `sys_write` + `sys_exit` |
| Ring 3 越权探针 | 启动自动 / `ring3probe` | 读内核地址触发 `#PF`，被安全回收 |
| 磁盘 / 分区 | `lsblk` / `df` | MBR 解析、块设备列表、SRFS 使用率 |

### 当前临时改动（重要）

为了定位"实体机启动卡死"，仓库当前保留了两处临时调试改动；另有一处 RTC 安全修复（建议永久保留）：

1. **鼠标初始化被注释**（`kernel/main.c`）：

   ```c
   /* ---- 临时排查：注释掉 mouse_init()，确认 i8042 初始化是否在真机死锁 ----
   mouse_init();
   serial_puts("[SRCore] mouse ok\n");
   ---- 排查结束把上面两行取消注释即可 ---- */
   ```

   取消注释后鼠标才会工作。

2. **强制 1024×768**：
   - `limine.conf`：`resolution: 1024x768x32`
   - `kernel/gfx/gfx.c`：`GFX_FORCE_W 1024` / `GFX_FORCE_H 768`
   - 恢复高分辨率：把 limine 改回 `1920x1080x32`，并把 `GFX_FORCE_W/H` 改为 `GFX_MAX_WIDTH/HEIGHT`。

3. **RTC 等待超时（安全修复，建议保留）**：`rtc_wait_ready()` 最多轮询 100000 次，超时后继续读取，避免 CMOS 一直报忙导致死等。

### Limine 请求规则

- 所有 `limine_*_request` 只在 `kernel/boot/limine_requests.c` 定义一次；
- 其它文件只能 `#include "boot/limine_requests.h"` 使用 `g_hhdm_req` / `g_kaddr_req` / `g_memmap_req` / `g_fb_req`；
- 重复定义会让 Limine 在加载内核前 `PANIC`，表现为**串口零输出、屏幕停在 Limine 错误页**。

### 无头测试示例

```bash
qemu-system-x86_64 -cdrom build/StarRingOS.iso \
  -drive if=none,id=disk0,file=build/disk.img,format=raw \
  -device ich9-ahci,id=sata -device ide-hd,drive=disk0,bus=sata.0 \
  -boot d -m 512M -display none -serial stdio -no-reboot
```

登录页操作：锁屏页按任意键（或双击）→ 密码页；默认用户 `guest` 密码为空，再按一次 Enter 即可进入桌面。无头环境可通过 QEMU monitor `sendkey` 注入。

---

## 已知限制与路线图

**架构限制**

- 单核、无 SMP；没有抢占式调度器。`task/task.c` 只是"任务登记表"，用于任务管理器展示，不参与 CPU 调度。
- 桌面是单线程事件循环：`on_click` / `on_hover` / `on_key` 运行在 PS/2 中断上下文，耗时工作必须通过 `gui_post_action()` 投递到主循环。
- 没有网络、音频、USB 存储、FAT32、ELF 用户进程加载器。
- 登录/密码只是演示，不是安全机制；文件系统没有权限位。
- 窗口不能自由拖拽缩放（只能最大化/还原）。
- 当前鼠标初始化被临时注释。

**可能的下一步**

- 恢复鼠标并完成真机输入验证；
- 实现 FAT32 驱动（VFS 已预留 `filesystem_ops`）；
- 用户态 ELF 加载器与更多 syscall；
- SMP / APIC 定时器；
- 更完整的窗口管理（缩放、最小化、Alt+Tab）；
- 网络栈（e1000 / virtio-net）。

---

## 故障排查

| 现象 | 可能原因 / 处理 |
| --- | --- |
| 串口零输出，屏幕停在 Limine | Limine 配置/内核加载失败；常见是**重复 request ID**（`PANIC: Conflict detected for request ID`）。检查是否在 `boot/limine_requests.c` 之外又声明了 `limine_*_request` |
| 有 Limine 菜单但进不去系统 | 确认 `limine.conf` 的 `path: boot():/boot/StarRingOS.elf` 与 ISO 内文件一致；`resolution:` 必须放在 entry 内（缩进） |
| VMware 下 AHCI 读写超时 | 控制器可能未开启 Bus Master；驱动已在 PCI 扫描时显式置位 `MEM_SPACE + BUS_MASTER`。仍失败可查看串口 `TFD/SERR` 日志 |
| 启动后提示 no filesystem | 数据盘为空或没有 SRFS 分区。空白 `disk0` 会自动建 MBR；否则打开 Disk Utility 格式化，或 `make disk` 重建示例盘 |
| 文件写入成功但重启后丢失 | 检查 `fs_sync()` / `FLUSH CACHE EXT` 日志；QEMU 需正常关机或等待回写 |
| 4K / 高分辨率屏画面模糊或被拉伸 | 当前逻辑分辨率被强制 1024×768，上屏时会缩放；若改回高分辨率，确认 `GFX_MAX_WIDTH/HEIGHT` 与显存带宽 |
| QEMU 启动很慢 | 没有 KVM 时使用 TCG 纯软件模拟，属正常现象 |
| 鼠标不动 | 当前 `mouse_init()` 被临时注释；取消注释并重新构建 |
| 登录后没有桌面 | 查看串口最后一条 `[SRCore]` 日志（如 `gui ok`、`desktop ready`），定位卡死步骤 |

---

## 致谢

- [Limine](https://github.com/limine-bootloader/limine)：现代 x86_64 引导器与协议。
- [SSFN（Scalable Screen Font）](https://gitlab.com/bztsrc/scalable-font2)：矢量字体渲染（`kernel/gfx/fontdata/ssfn.h` 与 `font.sfn`）。
- QEMU / VMware 提供的虚拟硬件环境。

> 仓库目前未附带 `LICENSE` 文件；如需开源发布，请先补充许可证。

---

**StarRing OS / SRCore** —— 从引导扇区到桌面，全部自己写。
