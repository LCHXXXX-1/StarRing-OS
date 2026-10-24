#include "shell.h"
#include "gfx/console.h"
#include "fs/fs.h"
#include "fs/partition.h"
#include "drivers/blkdev.h"
#include "arch/x86_64/syscall.h"
#include "lib/string.h"
#include "apps/files.h"
#include "apps/disk_utility.h"
#include "apps/taskmgr.h"
#include "apps/sysmon.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "task/task.h"

/* ---------- 工具 ---------- */

static void print_err(const char *op, const char *path, int err) {
    console_puts(op);
    console_puts(": ");
    if (path && path[0]) {
        console_puts(path);
        console_puts(": ");
    }
    console_puts(fs_strerror(err));
    console_putc('\n');
}

static void join_path(char *out, const char *dir, const char *name) {
    size_t n = 0;
    for (size_t i = 0; dir[i] && n + 1 < FS_PATH_MAX; i++) out[n++] = dir[i];
    if (n == 0 || out[n - 1] != '/')
        if (n + 1 < FS_PATH_MAX) out[n++] = '/';
    for (size_t i = 0; name[i] && n + 1 < FS_PATH_MAX; i++) out[n++] = name[i];
    out[n] = 0;
}

static const char *base_of(const char *path) {
    const char *b = path;
    for (const char *s = path; *s; s++)
        if (*s == '/' && s[1]) b = s + 1;
    return b;
}

/* ---------- 命令 ---------- */

static void cmd_help(void) {
    console_puts("可用命令：\n"
                 "  help            显示本帮助\n"
                 "  ls [路径]       列出目录内容\n"
                 "  cat <文件>      显示文件内容\n"
                 "  stat <路径>     显示 inode 信息\n"
                 "  mkdir <目录>    创建目录\n"
                 "  touch <文件>    创建空文件\n"
                 "  cp <源> <目标>  复制文件\n"
                 "  rm <路径>       删除文件或空目录\n"
                 "  cd <路径>       切换目录\n"
                 "  pwd             显示当前目录\n"
                 "  echo [文本] > f 把文本写入文件\n"
                 "  df              磁盘 / inode 使用率\n"
                 "  lsblk           显示 MBR 分区表\n"
                 "  mem             分页 / 物理内存统计\n"
                 "  ptest           运行页表映射自检\n"
                 "  tasks           列出已登记任务\n"
                 "  taskmgr         打开任务管理器窗口\n"
                 "  sysmon          打开系统监视器（内存）窗口\n"
                 "  files [路径]    打开文件管理器（此电脑）\n"
                 "  format          打开磁盘工具（格式化分区）\n"
                 "  ring3           运行 Ring 3 Hello 程序\n"
                 "  ring3probe      验证 Ring 3 无法读取内核内存\n"
                 "  clear           清屏\n"
                 "\n"
                 "行编辑：左/右/Home/End，Backspace/Delete，\n"
                 "        上/下 浏览历史。\n");
}

static void cmd_ls(int argc, char **argv) {
    if (!fs_ready()) {
        console_puts("ls: 未挂载文件系统");
        if (!partition_valid())
            console_puts("（无 MBR 分区表）");
        else if (partition_count() == 0)
            console_puts("（MBR 没有可用分区）");
        console_puts("\n");
        if (partition_count() == 0)
            console_puts("提示：运行 'format' 打开磁盘工具，初始化一块空盘。\n");
        return;
    }
    const char *path = (argc > 1) ? argv[1] : fs_getcwd();
    static struct fs_dirent ents[FS_MAX_DIRENTS];
    int n = fs_listdir(path, ents, FS_MAX_DIRENTS);
    if (n < 0) { print_err("ls", path, n); return; }
    if (n == 0) { console_puts("(空目录)\n"); return; }

    char buf[24];
    for (int i = 0; i < n; i++) {
        if (ents[i].type == FS_TYPE_DIR) console_puts("[目录] ");
        else                             console_puts("[文件] ");
        console_puts(ents[i].name);
        if (ents[i].type == FS_TYPE_FILE) {
            console_puts("  (");
            console_puts(utoa(ents[i].size, buf, 10));
            console_puts(" 字节)");
        }
        console_putc('\n');
    }
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) { console_puts("用法: cat <文件>\n"); return; }
    static char buf[512];
    uint64_t off = 0;
    for (;;) {
        int r = fs_read(argv[1], buf, sizeof(buf), off);
        if (r < 0) { print_err("cat", argv[1], r); return; }
        if (r == 0) break;
        for (int i = 0; i < r; i++) console_putc(buf[i]);
        off += (uint64_t)r;
    }
    if (off == 0 || buf[(off - 1) % sizeof(buf)] != '\n') console_putc('\n');
}

static void cmd_stat(int argc, char **argv) {
    if (argc < 2) { console_puts("用法: stat <路径>\n"); return; }
    struct fs_dirent st;
    int r = fs_stat(argv[1], &st);
    if (r < 0) { print_err("stat", argv[1], r); return; }
    char b[24];
    console_puts("  名称:  "); console_puts(st.name); console_putc('\n');
    console_puts("  类型:  ");
    console_puts(st.type == FS_TYPE_DIR ? "目录" : "文件");
    console_putc('\n');
    console_puts("  inode: "); console_puts(utoa(st.ino, b, 10)); console_putc('\n');
    console_puts("  大小:  "); console_puts(utoa(st.size, b, 10));
    console_puts(" 字节\n");
}

static void cmd_mkdir(int argc, char **argv) {
    if (argc < 2) { console_puts("用法: mkdir <目录>\n"); return; }
    int r = fs_create(argv[1], FS_TYPE_DIR);
    if (r < 0) print_err("mkdir", argv[1], r);
}

static void cmd_touch(int argc, char **argv) {
    if (argc < 2) { console_puts("用法: touch <文件>\n"); return; }
    if (fs_exists(argv[1])) return;
    int r = fs_create(argv[1], FS_TYPE_FILE);
    if (r < 0) print_err("touch", argv[1], r);
}

static void cmd_cp(int argc, char **argv) {
    if (argc < 3) { console_puts("用法: cp <源> <目标>\n"); return; }

    struct fs_dirent st;
    int r = fs_stat(argv[1], &st);
    if (r < 0) { print_err("cp", argv[1], r); return; }
    if (st.type == FS_TYPE_DIR) { console_puts("cp: 源是目录\n"); return; }

    char dst[FS_PATH_MAX];
    struct fs_dirent d;
    if (fs_stat(argv[2], &d) == FS_OK && d.type == FS_TYPE_DIR)
        join_path(dst, argv[2], base_of(argv[1]));
    else
        kstrcpy(dst, argv[2]);

    if (fs_exists(dst)) {
        r = fs_write_file(dst, "", 0);      /* 截断已存在的目标 */
        if (r < 0) { print_err("cp", dst, r); return; }
    } else {
        r = fs_create(dst, FS_TYPE_FILE);
        if (r < 0) { print_err("cp", dst, r); return; }
    }

    static char buf[512];
    uint64_t off = 0;
    for (;;) {
        int n = fs_read(argv[1], buf, sizeof(buf), off);
        if (n < 0) { print_err("cp", argv[1], n); return; }
        if (n == 0) break;
        int w = fs_write(dst, buf, (uint64_t)n, off);
        if (w < 0) { print_err("cp", dst, w); return; }
        off += (uint64_t)n;
    }
}

static void cmd_rm(int argc, char **argv) {
    if (argc < 2) { console_puts("用法: rm <路径>\n"); return; }
    int r = fs_remove(argv[1]);
    if (r < 0) print_err("rm", argv[1], r);
}

static void cmd_cd(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "/";
    int r = fs_chdir(path);
    if (r < 0) print_err("cd", path, r);
}

static void cmd_pwd(void) {
    console_puts(fs_getcwd());
    console_putc('\n');
}

static void cmd_df(void) {
    uint64_t tb, fb, ti, fi;
    int r = fs_statfs(&tb, &fb, &ti, &fi);
    if (r < 0) { print_err("df", "", r); return; }
    char b1[24], b2[24], b3[24], b4[24], b5[24];
    console_puts("SRFS  块  已用 ");
    console_puts(utoa(tb - fb, b1, 10));
    console_puts("，空闲 ");
    console_puts(utoa(fb, b2, 10));
    console_puts("，总计 ");
    console_puts(utoa(tb * 4096 / 1024 / 1024, b5, 10));
    console_puts(" MB\n");
    console_puts("      inode 已用 ");
    console_puts(utoa(ti - fi, b3, 10));
    console_puts("，空闲 ");
    console_puts(utoa(fi, b4, 10));
    console_putc('\n');
}

static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) console_putc(' ');
        console_puts(argv[i]);
    }
    console_putc('\n');
}

/* 打开图形界面的文件管理器（资源管理器）。path 省略时从根目录开始。 */
static void cmd_files(int argc, char **argv) {
    files_open((argc > 1) ? argv[1] : "/");
    console_puts("文件管理器：已在窗口中打开。\n");
}

/* 打开图形界面的磁盘格式化工具。真正的擦除发生在窗口里的二次确认之后。 */
static void cmd_format(void) {
    disk_utility_open();
    console_puts("磁盘工具：已在窗口中打开。\n");
}

/* ---------- 内存 / 分页 / 任务 ---------- */
static void put_num(uint64_t v) { char b[24]; console_puts(utoa(v, b, 10)); }

static void put_hex(uint64_t v) {
    static const char *d = "0123456789ABCDEF";
    console_puts("0x");
    for (int i = 0; i < 16; i++) console_putc(d[(v >> (60 - i * 4)) & 0xF]);
}

/* 有界字符串追加（shell 里打印表格用） */
static void s_app(char *dst, const char *src, uint32_t cap) {
    uint32_t n = 0;
    while (n + 1 < cap && dst[n]) n++;
    while (*src && n + 1 < cap) dst[n++] = *src++;
    dst[n] = 0;
}

/* 显示宽度（8px 字符格）：ASCII 1 格，CJK 2 格。表格对齐必须按格算，
   否则中文表头会把后面的列推歪。 */
static int txt_cells(const char *s) {
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x80) n += 1;
        else if ((*p & 0xC0) == 0x80) continue;   /* UTF-8 续字节 */
        else n += 2;
    }
    return n;
}

static void put_pad(const char *s, int width) {
    console_puts(s);
    int n = txt_cells(s);
    while (n < width) { console_putc(' '); n++; }
}

/* 打印所有 AHCI 盘上的 MBR 分区，并标出根卷/已挂载卷。 */
static void cmd_lsblk(void) {
    if (!partition_valid()) {
        console_puts("lsblk: 所有 AHCI 盘上都没有有效 MBR (0x55AA)\n");
        return;
    }

    console_puts("设备  ");
    put_pad("类型", 14);
    put_pad("起始LBA", 12);
    put_pad("扇区数", 12);
    put_pad("大小", 8);
    put_pad("状态", 10);
    console_putc('\n');

    char b[24], dev[12], type[24], size[16];
    for (int i = 0; i < partition_count(); i++) {
        const struct partition_info *p = partition_get(i);

        kstrcpy(dev, "sd");
        char letter[2] = { (char)('a' + (p->disk_idx % 26)), 0 };
        s_app(dev, letter, sizeof(dev));
        s_app(dev, utoa(p->index, b, 10), sizeof(dev));
        put_pad(dev, 6);

        kstrcpy(type, "0x");
        s_app(type, utoa(p->type, b, 16), sizeof(type));
        s_app(type, " ", sizeof(type));
        s_app(type, partition_type_name(p->type), sizeof(type));
        put_pad(type, 14);

        put_pad(utoa(p->start_lba, b, 10), 12);
        put_pad(utoa(p->sectors, b, 10), 12);

        kstrcpy(size, utoa((uint64_t)p->sectors * 512u / 1048576u, b, 10));
        s_app(size, " MB", sizeof(size));
        put_pad(size, 8);

        const struct block_device *bd =
            blkdev_find_part(p->disk_idx, p->start_lba);
        int bi = blkdev_index_of(bd);
        if (fs_ready() && bi == fs_root_bdev_index())
            console_puts("系统（已挂载）");
        else if (bi >= 0 && fs_bdev_is_mounted(bi))
            console_puts("已挂载");
        else
            console_puts("-");
        console_putc('\n');
    }
    console_puts("（共 ");
    put_num((uint64_t)partition_count());
    console_puts(" 个主分区）\n");
}

static void cmd_mem(void) {
    console_puts("PMM（物理页帧，4 KiB）：\n");
    if (pmm_ready()) {
        console_puts("  页帧  总计 "); put_num(pmm_total_frames());
        console_puts("  空闲 "); put_num(pmm_free_frames());
        console_puts("  已用 "); put_num(pmm_used_frames()); console_putc('\n');
        console_puts("  内存  总计 "); put_num(pmm_total_frames() * 4096 / 1048576u);
        console_puts(" MB  空闲 "); put_num(pmm_free_frames() * 4096 / 1048576u);
        console_puts(" MB\n");
    } else {
        console_puts("  未初始化\n");
    }

    console_puts("VMM（分页，当前 PML4）：\n");
    if (vmm_ready()) {
        console_puts("  CR3             "); put_hex(vmm_cr3()); console_putc('\n');
        console_puts("  HHDM 偏移       "); put_hex(vmm_hhdm()); console_putc('\n');
        console_puts("  已映射页        "); put_num(vmm_mapped_pages()); console_putc('\n');
        console_puts("  页表帧          "); put_num(vmm_table_frames()); console_putc('\n');
    } else {
        console_puts("  未初始化\n");
    }
}

static void cmd_ptest(void) {
    console_puts("分页自检: ");
    if (!pmm_ready() || !vmm_ready()) {
        console_puts("跳过（PMM/VMM 未就绪）\n");
        return;
    }
    console_puts(vmm_selftest() ? "通过\n" : "失败\n");
}

static void cmd_tasks(void) {
    struct task_info list[TASK_MAX];
    int n = task_snapshot(list, TASK_MAX);
    console_puts("PID  类型      名称\n");
    for (int i = 0; i < n; i++) {
        put_num((uint64_t)list[i].pid);
        console_puts(list[i].kind == TASK_KIND_APP ? "    应用      " : "    系统      ");
        console_puts(list[i].name);
        console_putc('\n');
    }
    console_puts("（共 "); put_num((uint64_t)n); console_puts(" 个任务）\n");
}

static void cmd_taskmgr(void) {
    taskmgr_open();
    console_puts("任务管理器：已在窗口中打开。\n");
}

static void cmd_sysmon(void) {
    sysmon_open();
    console_puts("系统监视器：已在窗口中打开。\n");
}

/* ---------- 入口 ---------- */

void shell_execute(char *cmdline) {
    char *argv[16];
    int argc = 0;
    char *p = cmdline;
    while (*p && argc < 16) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    if (argc == 0) return;

    /* "echo ... > file" 重定向 */
    for (int i = 1; i < argc - 1; i++) {
        if (kstrcmp(argv[i], ">") != 0) continue;
        if (kstrcmp(argv[0], "echo") != 0) {
            console_puts("重定向仅支持 echo\n");
            return;
        }
        if (i + 2 != argc) {
            console_puts("用法: echo [文本] > 文件\n");
            return;
        }
        char content[256];
        int cl = 0;
        for (int j = 1; j < i; j++) {
            if (j > 1) content[cl++] = ' ';
            for (const char *s = argv[j]; *s && cl < 254; s++)
                content[cl++] = *s;
        }
        content[cl++] = '\n';
        content[cl] = 0;
        int r = fs_write_file(argv[argc - 1], content, (uint64_t)cl);
        if (r < 0) print_err("echo", argv[argc - 1], r);
        return;
    }

    if (kstrcmp(argv[0], "help") == 0)       cmd_help();
    else if (kstrcmp(argv[0], "ls") == 0)    cmd_ls(argc, argv);
    else if (kstrcmp(argv[0], "cat") == 0)   cmd_cat(argc, argv);
    else if (kstrcmp(argv[0], "stat") == 0)  cmd_stat(argc, argv);
    else if (kstrcmp(argv[0], "mkdir") == 0) cmd_mkdir(argc, argv);
    else if (kstrcmp(argv[0], "touch") == 0) cmd_touch(argc, argv);
    else if (kstrcmp(argv[0], "cp") == 0)    cmd_cp(argc, argv);
    else if (kstrcmp(argv[0], "rm") == 0)    cmd_rm(argc, argv);
    else if (kstrcmp(argv[0], "cd") == 0)    cmd_cd(argc, argv);
    else if (kstrcmp(argv[0], "pwd") == 0)   cmd_pwd();
    else if (kstrcmp(argv[0], "df") == 0)    cmd_df();
    else if (kstrcmp(argv[0], "lsblk") == 0) cmd_lsblk();
    else if (kstrcmp(argv[0], "mem") == 0)   cmd_mem();
    else if (kstrcmp(argv[0], "ptest") == 0) cmd_ptest();
    else if (kstrcmp(argv[0], "tasks") == 0) cmd_tasks();
    else if (kstrcmp(argv[0], "taskmgr") == 0) cmd_taskmgr();
    else if (kstrcmp(argv[0], "sysmon") == 0) cmd_sysmon();
    else if (kstrcmp(argv[0], "files") == 0 ||
             kstrcmp(argv[0], "explorer") == 0) cmd_files(argc, argv);
    else if (kstrcmp(argv[0], "format") == 0) cmd_format();
    else if (kstrcmp(argv[0], "ring3") == 0) {
        if (usermode_run_demo()) console_puts("ring3: Hello 程序执行完毕。\n");
        else                     console_puts("ring3: 执行失败。\n");
    }
    else if (kstrcmp(argv[0], "ring3probe") == 0) {
        if (usermode_run_probe()) console_puts("ring3probe: 越权访问已被隔离。\n");
        else                      console_puts("ring3probe: 隔离验证失败。\n");
    }
    else if (kstrcmp(argv[0], "clear") == 0) console_clear();
    else if (kstrcmp(argv[0], "echo") == 0)  cmd_echo(argc, argv);
    else {
        console_puts(argv[0]);
        console_puts(": 未找到命令，输入 'help' 查看帮助\n");
    }
}
