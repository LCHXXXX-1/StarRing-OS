#include "disk_utility.h"
#include "gfx/gui.h"
#include "fs/fs.h"
#include "fs/vfs.h"
#include "fs/partition.h"
#include "drivers/ahci.h"
#include "drivers/blkdev.h"
#include "drivers/keyboard.h"
#include "drivers/power.h"
#include "drivers/serial.h"
#include "apps/files.h"
#include "lib/mem.h"
#include "lib/string.h"

/* ============================================================
 *  Disk Utility —— 分区管理器 / 格式化工具
 *
 *  界面（ST_MAIN）：
 *    顶部  : 物理磁盘信息 "Disk 0: 64 MB - MBR"
 *    中部  : 分区列表（# / START LBA / SIZE / TYPE / STATUS），可点击选中
 *    底部  : 红色安全提示 + [Format Selected Partition] [Cancel]
 *
 *  安全规则：
 *    - 选中当前根文件系统所在分区时，格式化按钮被禁用，并显示红字
 *      "无法格式化正在使用的系统分区！"；
 *    - 格式化前必须经过二次确认；
 *    - 只格式化用户选中的分区，绝不传 0 去覆盖整块盘（LBA 0 是 MBR）。
 *
 *  状态机（d->state）：
 *    ST_MAIN       列表 / 选择
 *    ST_CONFIRM    二次确认
 *    ST_FORMATTING 真实写盘进度条
 *    ST_DONE       完成/失败
 *
 *  两个关键约束：
 *   1) 禁止 malloc —— 整个应用只有一个文件级静态实例 du。
 *   2) 鼠标回调 on_click 运行在 PS/2 IRQ1 中断上下文。格式化是一串
 *      AHCI 写 + FLUSH 的长任务，在中断里跑会拖住键盘/鼠标节拍。
 *      所以 [Yes, Erase] 只切状态并 gui_post_action() 把工作投递给
 *      桌面主循环；主循环里再用 gui_repaint() 同步刷帧。
 *
 *  进度条是真实的：fs_format_bdev() 每写一个元数据块回调一次
 *  du_progress()，FLUSH 落盘后再报 100%。没有任何 busy_wait 假动画。
 * ============================================================ */

#define DU_W    580
#define DU_H    400
#define DU_X    230
#define DU_Y    130

/* SRFS 块大小是 fs.c 的内部常量 4096（fs_statfs 的单位） */
#define DU_BLOCK_BYTES   4096u

/* 分区表几何（内容区相对坐标） */
#define DU_TBL_X         16
#define DU_TBL_Y         74
#define DU_TBL_HDR_H     20
#define DU_ROW_H         30
#define DU_TBL_ROWS      5      /* 窗口内同时显示的行数，超出用 scroll */

/* 按钮 */
#define DU_BTN_W         190
#define DU_BTN_H         28
#define DU_BTN_GAP       12
#define DU_BTN_BOTTOM    18

#define DU_BAR_H         16
#define DU_PANEL_H       54

/* 颜色 */
#define DU_RED_TEXT      0x00FF6B5E
#define DU_RED_BG        0x008E2F26
#define DU_RED_HOVER     GUI_COL_CLOSE_HOVER
#define DU_RED_EDGE      0x00F0705F
#define DU_RED_PANEL     0x003A1613
#define DU_OK_TEXT       0x007BE08A
#define DU_WARN_TEXT     0x00F2C14E
#define DU_BTN_EDGE      0x003A5C80

enum { ST_MAIN = 0, ST_CONFIRM, ST_FORMATTING, ST_DONE };
enum { SLOT_NONE = -1, SLOT_LEFT = 0, SLOT_RIGHT = 1 };

/* 表格中的一行：物理盘标题行（part_idx < 0，可初始化）或分区行。 */
struct du_row {
    int disk_idx;
    int part_idx;               /* partition_get 下标；-1 表示“整盘”行 */
};

struct disk_util {
    struct gui_window *win;
    int      state;
    int      hbtn;              /* 悬停中的按钮槽位 */
    int      hpart;             /* 悬停中的表格行（-1 无） */
    int      focus;             /* 键盘焦点按钮槽位 */
    int      sel;               /* 选中的表格行 */
    int      scroll;            /* 表格首行 */
    int      row_n;
    struct du_row rows[PART_MAX + PART_MAX_DISKS];
    int      progress;          /* 0..100 */
    int      result;            /* fs_format_bdev() 的返回值 */
    uint32_t fmt_index;         /* 正在格式化的 MBR 槽位号（1..4） */
    uint32_t fmt_start;         /* 正在格式化的分区起始 LBA */
    int      fmt_disk;          /* 目标物理盘 */
    int      fmt_bdev;          /* 目标 block_device 下标 */
    bool     fmt_was_active;    /* 被格式化的分区是否是活动根分区 */
    bool     init_mode;         /* 初始化空白盘（写 MBR + 建分区），而非格式化已有分区 */
    bool     mounted;           /* 初始化后已立即挂载，无需重启 */
    char     info[72];          /* "Disk 0: 64 MB - MBR" */
    char     detail[96];        /* 第二行：分区数 / 活动挂载点 */
};

/* 单实例，静态分配 */
static struct disk_util du;
/* 正在主循环里跑格式化：期间吞掉所有交互，防止重入 */
static volatile bool du_busy;

static void du_run_format(void);
static void du_run_init(void);

/* ---------------- 小工具 ---------------- */

static void cat(char *dst, const char *src, uint32_t cap) {
    uint32_t l = 0;
    while (l + 1 < cap && dst[l]) l++;
    uint32_t i = 0;
    while (src[i] && l + 1 < cap) dst[l++] = src[i++];
    dst[l] = 0;
}

static void cat_num(char *dst, uint64_t v, uint32_t cap) {
    char b[24];
    cat(dst, utoa(v, b, 10), cap);
}

static void hex_byte(uint8_t v, char *out) {
    static const char *d = "0123456789ABCDEF";
    out[0] = '0';
    out[1] = 'x';
    out[2] = d[v >> 4];
    out[3] = d[v & 15];
    out[4] = 0;
}

static const struct du_row *du_sel_row(const struct disk_util *d) {
    if (d->sel < 0 || d->sel >= d->row_n) return 0;
    return &d->rows[d->sel];
}

static const struct partition_info *du_sel_part(const struct disk_util *d) {
    const struct du_row *r = du_sel_row(d);
    if (!r || r->part_idx < 0) return 0;
    return partition_get(r->part_idx);
}

static bool du_is_active(const struct partition_info *p) {
    if (!p || !fs_ready()) return false;
    const struct block_device *b = blkdev_find_part(p->disk_idx, p->start_lba);
    int idx = blkdev_index_of(b);
    return idx >= 0 && idx == fs_root_bdev_index();
}

/* 只有 SRFS / Linux 兼容分区允许格式化；FAT/USB 等保持只读。 */
static bool du_part_srfs_compat(const struct partition_info *p) {
    return p && (p->type == PART_TYPE_SRFS || p->type == PART_TYPE_LINUX);
}

static void du_build_rows(struct disk_util *d) {
    d->row_n = 0;
    for (int di = 0; di < partition_disk_count(); di++) {
        const struct partition_disk *disk = partition_disk_info(di);
        if (!disk) continue;
        int before = d->row_n;
        for (int i = 0; i < partition_count(); i++) {
            const struct partition_info *p = partition_get(i);
            if (!p || p->disk_idx != di) continue;
            if (d->row_n >= (int)(sizeof(d->rows) / sizeof(d->rows[0]))) break;
            d->rows[d->row_n].disk_idx = di;
            d->rows[d->row_n].part_idx = i;
            d->row_n++;
        }
        if (d->row_n == before) {
            if (d->row_n >= (int)(sizeof(d->rows) / sizeof(d->rows[0]))) continue;
            d->rows[d->row_n].disk_idx = di;
            d->rows[d->row_n].part_idx = -1;
            d->row_n++;
        }
    }
    if (d->sel >= d->row_n) d->sel = d->row_n - 1;
    if (d->sel < 0 && d->row_n > 0) d->sel = 0;
    if (d->scroll > d->sel) d->scroll = d->sel;
    if (d->sel >= d->scroll + DU_TBL_ROWS)
        d->scroll = d->sel - DU_TBL_ROWS + 1;
    if (d->scroll < 0) d->scroll = 0;
}

static uint64_t du_part_mb(const struct partition_info *p) {
    return (uint64_t)p->sectors * 512u / 1048576u;
}

static void du_update_info(struct disk_util *d) {
    d->info[0] = 0;
    d->detail[0] = 0;

    if (!ahci_present() || partition_disk_count() == 0) {
        kstrcpy(d->info, "未检测到 AHCI 磁盘");
        kstrcpy(d->detail, "请连接 SATA/AHCI 磁盘并重新打开磁盘工具");
        return;
    }

    const struct du_row *r = du_sel_row(d);
    int di = r ? r->disk_idx : 0;
    const struct partition_disk *disk = partition_disk_info(di);
    if (!disk) disk = partition_disk_info(0);
    if (!disk) return;

    kstrcpy(d->info, "磁盘 ");
    cat_num(d->info, (uint64_t)disk->ahci_idx, sizeof(d->info));
    cat(d->info, ": ", sizeof(d->info));
    cat_num(d->info, disk->sectors * 512u / 1048576u, sizeof(d->info));
    cat(d->info, " MB", sizeof(d->info));
    if (disk->mbr_valid) cat(d->info, " - MBR", sizeof(d->info));

    cat_num(d->detail, (uint64_t)disk->part_count, sizeof(d->detail));
    cat(d->detail, " 个主分区", sizeof(d->detail));
    if (!disk->mbr_valid)
        kstrcpy(d->detail, "没有 MBR 分区表");
    else if (disk->part_count == 0)
        kstrcpy(d->detail, "MBR 没有分区");

    cat(d->detail, "  |  ", sizeof(d->detail));
    cat_num(d->detail, (uint64_t)vfs_mount_count(), sizeof(d->detail));
    cat(d->detail, " 个卷已挂载", sizeof(d->detail));
    if (fs_ready()) {
        int ri = fs_root_bdev_index();
        const struct block_device *rb = (ri >= 0) ? blkdev_get(ri) : 0;
        cat(d->detail, " | 根目录: ", sizeof(d->detail));
        if (rb) {
            cat(d->detail, rb->name, sizeof(d->detail));
        } else {
            cat(d->detail, "未知", sizeof(d->detail));
        }
    }
}

/* ---------------- 几何 ---------------- */

static void du_btn_rect(uint32_t cw, uint32_t ch, int slot,
                        int32_t *x, int32_t *y, uint32_t *w, uint32_t *h) {
    int32_t total = (int32_t)(DU_BTN_W * 2 + DU_BTN_GAP);
    int32_t x0    = ((int32_t)cw - total) / 2;
    *y = (int32_t)ch - DU_BTN_BOTTOM - DU_BTN_H;
    *w = DU_BTN_W;
    *h = DU_BTN_H;
    *x = (slot == SLOT_LEFT) ? x0 : x0 + (int32_t)DU_BTN_W + DU_BTN_GAP;
}

static void du_row_rect(uint32_t cw, int i,
                        int32_t *x, int32_t *y, uint32_t *w, uint32_t *h) {
    *x = DU_TBL_X;
    *y = DU_TBL_Y + DU_TBL_HDR_H + i * DU_ROW_H;
    *w = (cw > 32) ? (cw - 32) : 32;
    *h = DU_ROW_H - 4;
}

static const char *du_btn_label(struct disk_util *d, int slot) {
    switch (d->state) {
    case ST_MAIN:
        if (slot == SLOT_LEFT) {
            const struct du_row *r = du_sel_row(d);
            return (r && r->part_idx < 0) ? "初始化磁盘 (MBR)"
                                          : "格式化所选分区";
        }
        return "取消";
    case ST_CONFIRM:
        if (slot == SLOT_LEFT)
            return d->init_mode ? "是，初始化" : "是，擦除";
        return "取消";
    case ST_DONE:    return (slot == SLOT_LEFT) ? "关闭"      : "立即重启";
    default:         return 0;                   /* 格式化中没有按钮 */
    }
}

static bool du_btn_enabled(struct disk_util *d, int slot) {
    if (slot == SLOT_NONE) return false;
    if (d->state == ST_FORMATTING) return false;
    if (slot == SLOT_LEFT && (d->state == ST_MAIN || d->state == ST_CONFIRM)) {
        const struct du_row *r = du_sel_row(d);
        if (!r) return false;
        if (r->part_idx < 0) {
            const struct partition_disk *disk =
                partition_disk_info(r->disk_idx);
            return disk && disk->present &&
                   disk->sectors >= (uint64_t)PART_DEFAULT_START + 4096u;
        }
        const struct partition_info *p = du_sel_part(d);
        return du_part_srfs_compat(p) && !du_is_active(p);
    }
    return true;
}

static bool du_btn_danger(struct disk_util *d, int slot) {
    return slot == SLOT_LEFT &&
           (d->state == ST_MAIN || d->state == ST_CONFIRM);
}

static int du_hit_slot(uint32_t cw, uint32_t ch, struct disk_util *d,
                       int32_t cx, int32_t cy) {
    if (!du_btn_label(d, SLOT_LEFT)) return SLOT_NONE;
    for (int slot = 0; slot < 2; slot++) {
        int32_t x, y;
        uint32_t w, h;
        du_btn_rect(cw, ch, slot, &x, &y, &w, &h);
        if (gui_hit(cx, cy, x, y, w, h)) return slot;
    }
    return SLOT_NONE;
}

static int du_hit_row(uint32_t cw, uint32_t ch, int32_t cx, int32_t cy) {
    (void)ch;
    const struct disk_util *d = &du;
    int end = d->scroll + DU_TBL_ROWS;
    if (end > d->row_n) end = d->row_n;
    for (int i = d->scroll; i < end; i++) {
        int32_t x, y;
        uint32_t w, h;
        du_row_rect(cw, i - d->scroll, &x, &y, &w, &h);
        if (gui_hit(cx, cy, x, y, w, h)) return i;
    }
    return -1;
}

/* ---------------- 绘制 ---------------- */

static void du_text_center(int32_t cx, int32_t cy, uint32_t cw, int32_t rel_y,
                           const char *s, uint32_t fg, uint32_t bg, bool bold) {
    int32_t x = cx + ((int32_t)cw - (int32_t)gui_str_width(s)) / 2;
    if (bold) gui_text_bold(x, cy + rel_y, s, fg, bg);
    else      gui_text(x, cy + rel_y, s, fg, bg);
}

static void du_draw_btn(struct disk_util *d, int32_t cx, int32_t cy,
                        uint32_t cw, uint32_t ch, int slot) {
    const char *label = du_btn_label(d, slot);
    if (!label) return;

    int32_t x, y;
    uint32_t w, h;
    du_btn_rect(cw, ch, slot, &x, &y, &w, &h);
    x += cx; y += cy;

    bool enabled = du_btn_enabled(d, slot);
    bool danger  = du_btn_danger(d, slot);
    bool hot     = enabled && (d->hbtn == slot);

    uint32_t bg, bd, fg;
    if (!enabled) {
        bg = GUI_COL_BTN_DISABLED;
        bd = GUI_COL_BORDER_DARK;
        fg = GUI_COL_TEXT_DIM;
    } else if (danger) {
        bg = hot ? DU_RED_HOVER : DU_RED_BG;
        bd = DU_RED_EDGE;
        fg = GUI_COL_WHITE;
    } else {
        bg = hot ? GUI_COL_BTN_HOVER : GUI_COL_BTN;
        bd = DU_BTN_EDGE;
        fg = GUI_COL_WHITE;
    }

    gui_round_fill(x, y, w, h, bg, GUI_COL_WINDOW_BG);
    gui_frame(x, y, w, h, bd);
    gui_pixel(x, y, GUI_COL_WINDOW_BG);
    gui_pixel(x + (int32_t)w - 1, y, GUI_COL_WINDOW_BG);
    if (enabled && d->focus == slot && d->hbtn != slot)
        gui_frame(x + 1, y + 1, w - 2, h - 2, GUI_COL_ACCENT);
    gui_text(x + ((int32_t)w - (int32_t)gui_str_width(label)) / 2,
             y + ((int32_t)h - GUI_FONT_H) / 2, label, fg, bg);
}

static void du_draw_disk(struct disk_util *d, int32_t cx, int32_t cy,
                         uint32_t cw, int32_t rel_y) {
    int32_t px = cx + 20;
    uint32_t pw = (cw > 40) ? (cw - 40) : 40;

    gui_round_fill(px, cy + rel_y, pw, DU_PANEL_H, GUI_COL_PANEL,
                   GUI_COL_WINDOW_BG);
    gui_frame(px, cy + rel_y, pw, DU_PANEL_H, DU_BTN_EDGE);
    gui_draw_mini_icon(px + 12, cy + rel_y + (DU_PANEL_H - 16) / 2,
                       GUI_ICON_DISK);
    gui_text_bold(px + 40, cy + rel_y + 8, d->info,
                  GUI_COL_TEXT, GUI_COL_PANEL);
    gui_text(px + 40, cy + rel_y + 28, d->detail,
             GUI_COL_TEXT_DIM, GUI_COL_PANEL);
}

/* 多盘列表：每个物理盘的分区各占一行；没有分区的盘显示一条可初始化行。 */
static void du_draw_table(struct disk_util *d, int32_t cx, int32_t cy,
                          uint32_t cw, int32_t rel_y) {
    int32_t tx = cx + DU_TBL_X;
    int32_t ty = cy + rel_y;
    uint32_t tw = (cw > 32) ? (cw - 32) : 32;

    gui_fill(tx, ty, tw, DU_TBL_HDR_H, GUI_COL_PANEL_LIGHT);
    gui_frame(tx, ty, tw, DU_TBL_HDR_H, DU_BTN_EDGE);
    gui_text(tx + 10,  ty + 3, "磁盘",      GUI_COL_TEXT_DIM, GUI_COL_PANEL_LIGHT);
    gui_text(tx + 58,  ty + 3, "分区",      GUI_COL_TEXT_DIM, GUI_COL_PANEL_LIGHT);
    gui_text(tx + 112, ty + 3, "起始 LBA", GUI_COL_TEXT_DIM, GUI_COL_PANEL_LIGHT);
    gui_text(tx + 210, ty + 3, "大小",      GUI_COL_TEXT_DIM, GUI_COL_PANEL_LIGHT);
    gui_text(tx + 292, ty + 3, "类型",      GUI_COL_TEXT_DIM, GUI_COL_PANEL_LIGHT);
    gui_text(tx + 410, ty + 3, "状态",    GUI_COL_TEXT_DIM, GUI_COL_PANEL_LIGHT);

    if (d->row_n == 0) {
        int32_t by = ty + DU_TBL_HDR_H;
        gui_fill(tx, by, tw, 46, GUI_COL_PANEL);
        gui_frame(tx, by, tw, 46, DU_BTN_EDGE);
        du_text_center(cx, cy, cw, rel_y + DU_TBL_HDR_H + 15,
                       "未找到 AHCI 磁盘 / 分区。",
                       GUI_COL_TEXT_DIM, GUI_COL_PANEL, false);
        return;
    }

    char num[8], lba[24], size[20], type[28], szb[24];
    int end = d->scroll + DU_TBL_ROWS;
    if (end > d->row_n) end = d->row_n;

    for (int i = d->scroll; i < end; i++) {
        const struct du_row *r = &d->rows[i];
        const struct partition_info *p = (r->part_idx >= 0)
                                             ? partition_get(r->part_idx)
                                             : 0;
        int32_t rx, ry;
        uint32_t rw, rh;
        du_row_rect(cw, i - d->scroll, &rx, &ry, &rw, &rh);
        rx += cx;
        ry += cy;

        uint32_t bg;
        if (d->sel == i)       bg = GUI_COL_SELECT;
        else if (d->hpart == i) bg = GUI_COL_SELECT_HOVER;
        else                    bg = (i & 1) ? GUI_COL_ROW_ALT : GUI_COL_PANEL;

        bool active = du_is_active(p);
        uint32_t fg = (d->sel == i) ? GUI_COL_WHITE : GUI_COL_TEXT;

        gui_fill(rx, ry, rw, rh, bg);
        gui_frame(rx, ry, rw, rh, DU_BTN_EDGE);

        kstrcpy(num, utoa((uint64_t)r->disk_idx, lba, 10));
        gui_text(rx + 10, ry + 6, num, fg, bg);

        if (p) {
            kstrcpy(num, utoa(p->index, lba, 10));
            gui_text(rx + 58, ry + 6, num, fg, bg);
            gui_text(rx + 112, ry + 6, utoa(p->start_lba, lba, 10), fg, bg);
            kstrcpy(size, utoa(du_part_mb(p), szb, 10));
            cat(size, " MB", sizeof(size));
            gui_text(rx + 210, ry + 6, size, fg, bg);

            char hx[8];
            hex_byte(p->type, hx);
            kstrcpy(type, hx);
            cat(type, " ", sizeof(type));
            cat(type, partition_type_name(p->type), sizeof(type));
            gui_text(rx + 292, ry + 6, type, fg, bg);
            const char *status = active ? "系统"
                               : du_part_srfs_compat(p) ? "数据"
                                                         : "只读";
            gui_text(rx + 410, ry + 6, status,
                     active ? DU_WARN_TEXT : GUI_COL_TEXT_DIM, bg);
        } else {
            const struct partition_disk *disk = partition_disk_info(r->disk_idx);
            gui_text(rx + 58, ry + 6, "-", fg, bg);
            gui_text(rx + 112, ry + 6, "-", fg, bg);
            if (disk) {
                kstrcpy(size, utoa(disk->sectors * 512u / 1048576u, szb, 10));
                cat(size, " MB", sizeof(size));
            } else {
                kstrcpy(size, "?");
            }
            gui_text(rx + 210, ry + 6, size, fg, bg);
            gui_text(rx + 292, ry + 6,
                     (disk && disk->mbr_valid) ? "未分区" : "未初始化",
                     fg, bg);
            gui_text(rx + 410, ry + 6, "初始化",
                     (d->sel == i) ? GUI_COL_WHITE : DU_WARN_TEXT, bg);
        }
    }
}

static void du_draw_hint(struct disk_util *d, int32_t cx, int32_t cy,
                         uint32_t cw) {
    const struct du_row *r = du_sel_row(d);
    const struct partition_info *p = du_sel_part(d);

    if (!ahci_present() || !r) {
        du_text_center(cx, cy, cw, 250,
                       "未检测到 AHCI 磁盘。",
                       DU_RED_TEXT, GUI_COL_WINDOW_BG, true);
        du_text_center(cx, cy, cw, 272,
                       "请连接 SATA/AHCI 磁盘并重新打开磁盘工具。",
                       GUI_COL_TEXT_DIM, GUI_COL_WINDOW_BG, false);
        return;
    }

    if (!p) {
        const struct partition_disk *disk = partition_disk_info(r->disk_idx);
        char line[96];
        kstrcpy(line, "已选: 磁盘 ");
        cat_num(line, (uint64_t)r->disk_idx, sizeof(line));
        if (disk) {
            cat(line, "  |  ", sizeof(line));
            cat_num(line, disk->sectors * 512u / 1048576u, sizeof(line));
            cat(line, " MB", sizeof(line));
        }
        du_text_center(cx, cy, cw, 250, line, GUI_COL_TEXT,
                       GUI_COL_WINDOW_BG, false);
        du_text_center(cx, cy, cw, 272,
                       disk && disk->mbr_valid
                           ? "此 MBR 没有可用分区。"
                           : "此磁盘上没有 MBR 分区表。",
                       DU_WARN_TEXT, GUI_COL_WINDOW_BG, true);
        du_text_center(cx, cy, cw, 294,
                       "点击“初始化磁盘”创建一个 0x53 SRFS 分区。",
                       GUI_COL_TEXT_DIM, GUI_COL_WINDOW_BG, false);
        return;
    }

    char line[96];
    kstrcpy(line, "已选: 磁盘 ");
    cat_num(line, (uint64_t)r->disk_idx, sizeof(line));
    cat(line, " / 分区 ", sizeof(line));
    cat_num(line, p->index, sizeof(line));
    cat(line, "  |  起始 LBA ", sizeof(line));
    cat_num(line, p->start_lba, sizeof(line));
    cat(line, "  |  ", sizeof(line));
    cat_num(line, du_part_mb(p), sizeof(line));
    cat(line, " MB", sizeof(line));
    du_text_center(cx, cy, cw, 250, line, GUI_COL_TEXT,
                   GUI_COL_WINDOW_BG, false);

    if (du_is_active(p)) {
        du_text_center(cx, cy, cw, 272,
                       "无法格式化正在使用的系统分区！",
                       DU_RED_TEXT, GUI_COL_WINDOW_BG, true);
        du_text_center(cx, cy, cw, 294,
                       "它承载着正在运行的根文件系统。",
                       GUI_COL_TEXT_DIM, GUI_COL_WINDOW_BG, false);
    } else if (!du_part_srfs_compat(p)) {
        du_text_center(cx, cy, cw, 272,
                       "此分区类型以只读方式挂载。",
                       DU_WARN_TEXT, GUI_COL_WINDOW_BG, true);
        du_text_center(cx, cy, cw, 294,
                       "只能格式化 SRFS (0x53) 和 Linux (0x83) 分区。",
                       GUI_COL_TEXT_DIM, GUI_COL_WINDOW_BG, false);
    } else {
        du_text_center(cx, cy, cw, 272,
                       "格式化将清除此分区上的所有数据。",
                       DU_RED_TEXT, GUI_COL_WINDOW_BG, false);
        du_text_center(cx, cy, cw, 294,
                       "稍后需要确认。",
                       GUI_COL_TEXT_DIM, GUI_COL_WINDOW_BG, false);
    }
}

static void du_draw_confirm(struct disk_util *d, int32_t cx, int32_t cy,
                            uint32_t cw) {
    if (d->init_mode) {
        const struct du_row *r = du_sel_row(d);
        char line[96];
        kstrcpy(line, "初始化磁盘 ");
        cat_num(line, r ? (uint64_t)r->disk_idx : 0, sizeof(line));
        cat(line, " 并写入新的 MBR?", sizeof(line));
        du_text_center(cx, cy, cw, 250, line,
                       GUI_COL_WHITE, GUI_COL_WINDOW_BG, true);
        du_text_center(cx, cy, cw, 272,
                       "将在 LBA 2048 处创建一个 0x53 SRFS 分区。",
                       DU_WARN_TEXT, GUI_COL_WINDOW_BG, false);
        du_text_center(cx, cy, cw, 294,
                       "现有分区表将被覆盖。",
                       DU_RED_TEXT, GUI_COL_WINDOW_BG, false);
        return;
    }

    const struct partition_info *p = du_sel_part(d);
    const struct du_row *r = du_sel_row(d);
    if (!p) return;

    char line[96];
    kstrcpy(line, "擦除磁盘 ");
    cat_num(line, r ? (uint64_t)r->disk_idx : 0, sizeof(line));
    cat(line, " 分区 ", sizeof(line));
    cat_num(line, p->index, sizeof(line));
    cat(line, "（LBA ", sizeof(line));
    cat_num(line, p->start_lba, sizeof(line));
    cat(line, " (", sizeof(line));
    cat_num(line, du_part_mb(p), sizeof(line));
    cat(line, " MB）?", sizeof(line));
    du_text_center(cx, cy, cw, 250, line, GUI_COL_WHITE,
                   GUI_COL_WINDOW_BG, true);
    du_text_center(cx, cy, cw, 272,
                   "此分区上的所有数据将被永久销毁。",
                   DU_RED_TEXT, GUI_COL_WINDOW_BG, false);
    du_text_center(cx, cy, cw, 294, "此操作无法撤销。",
                   GUI_COL_TEXT_DIM, GUI_COL_WINDOW_BG, false);
}

static void du_draw_bar(struct disk_util *d, int32_t cx, int32_t cy,
                        uint32_t cw, int32_t rel_y) {
    int32_t bw = (int32_t)cw - 80;
    if (bw < 40) bw = 40;
    int32_t bx = cx + 40;
    int32_t by = cy + rel_y;

    gui_fill(bx, by, (uint32_t)bw, DU_BAR_H, GUI_COL_PANEL);
    gui_frame(bx, by, (uint32_t)bw, DU_BAR_H, DU_BTN_EDGE);

    int32_t p = d->progress;
    if (p < 0)   p = 0;
    if (p > 100) p = 100;
    int32_t fw = (int32_t)(((bw - 4) * p) / 100);
    if (fw > 0)
        gui_fill(bx + 2, by + 2, (uint32_t)fw, DU_BAR_H - 4, GUI_COL_ACCENT);

    char pct[12] = "";
    cat_num(pct, (uint64_t)p, sizeof(pct));
    cat(pct, "%", sizeof(pct));
    du_text_center(cx, cy, cw, rel_y + DU_BAR_H + 6, pct,
                   GUI_COL_TEXT_DIM, GUI_COL_WINDOW_BG, false);
}

static void du_draw_done(struct disk_util *d, int32_t cx, int32_t cy,
                         uint32_t cw) {
    if (d->result == FS_OK) {
        du_text_center(cx, cy, cw, 96,
                       (d->init_mode && d->mounted)
                           ? "磁盘已初始化并挂载。"
                           : (d->init_mode ? "磁盘已初始化。"
                                           : "格式化完成。"),
                       DU_OK_TEXT, GUI_COL_WINDOW_BG, true);

        char line[96];
        kstrcpy(line, "分区 ");
        cat_num(line, d->fmt_index, sizeof(line));
        if (d->init_mode)
            cat(line, " 现在是 SRFS 文件系统。", sizeof(line));
        else
            cat(line, " 现在包含一个空的 SRFS。", sizeof(line));
        du_text_center(cx, cy, cw, 124, line, GUI_COL_TEXT,
                       GUI_COL_WINDOW_BG, false);

        if (d->init_mode) {
            if (d->mounted) {
                du_text_center(cx, cy, cw, 152,
                               "文件系统已挂载 - 打开“此电脑”即可浏览。",
                               DU_OK_TEXT, GUI_COL_WINDOW_BG, false);
            } else {
                du_text_center(cx, cy, cw, 152,
                               "重启后即可挂载新文件系统。",
                               DU_WARN_TEXT, GUI_COL_WINDOW_BG, true);
            }
        } else if (d->fmt_was_active) {
            du_text_center(cx, cy, cw, 152,
                           "活动文件系统已被擦除 - 需要重启。",
                           DU_RED_TEXT, GUI_COL_WINDOW_BG, false);
        } else {
            du_text_center(cx, cy, cw, 152,
                           "正在运行的系统分区未被改动。",
                           GUI_COL_TEXT_DIM, GUI_COL_WINDOW_BG, false);
        }
    } else {
        char line[96];
        kstrcpy(line, d->init_mode ? "初始化失败: "
                                   : "格式化失败: ");
        cat(line, fs_strerror(d->result), sizeof(line));
        du_text_center(cx, cy, cw, 96, line, DU_RED_TEXT,
                       GUI_COL_WINDOW_BG, true);
        du_text_center(cx, cy, cw, 124,
                       d->init_mode
                           ? "此磁盘可能没有可用的文件系统。"
                           : "分区未被擦除。",
                       GUI_COL_TEXT_DIM, GUI_COL_WINDOW_BG, false);
    }
}

static void du_draw(struct gui_window *win) {
    struct disk_util *d = win->data;
    if (!d) return;

    int32_t cx = gui_content_x(win), cy = gui_content_y(win);
    uint32_t cw = gui_content_w(win), ch = gui_content_h(win);

    gui_fill(cx, cy, cw, ch, GUI_COL_WINDOW_BG);

    switch (d->state) {
    case ST_MAIN:
        du_draw_disk(d, cx, cy, cw, 12);
        du_draw_table(d, cx, cy, cw, DU_TBL_Y);
        du_draw_hint(d, cx, cy, cw);
        break;

    case ST_CONFIRM:
        du_draw_disk(d, cx, cy, cw, 12);
        du_draw_table(d, cx, cy, cw, DU_TBL_Y);
        du_draw_confirm(d, cx, cy, cw);
        break;

    case ST_FORMATTING:
        du_draw_disk(d, cx, cy, cw, 12);
        du_draw_table(d, cx, cy, cw, DU_TBL_Y);
        {
            char line[64];
            if (d->init_mode) {
                kstrcpy(line, "正在初始化磁盘 (MBR + SRFS)...");
            } else {
                kstrcpy(line, "正在格式化分区 ");
                cat_num(line, d->fmt_index, sizeof(line));
                cat(line, "...", sizeof(line));
            }
            du_text_center(cx, cy, cw, 250, line, GUI_COL_WHITE,
                           GUI_COL_WINDOW_BG, true);
        }
        du_draw_bar(d, cx, cy, cw, 278);
        du_text_center(cx, cy, cw, 322,
                       d->init_mode
                           ? "正在写入 MBR 和文件系统元数据 - 请稍候。"
                           : "正在写入文件系统元数据 - 请稍候。",
                       GUI_COL_TEXT_DIM, GUI_COL_WINDOW_BG, false);
        break;

    case ST_DONE:
        du_draw_disk(d, cx, cy, cw, 12);
        du_draw_done(d, cx, cy, cw);
        break;

    default:
        break;
    }

    if (d->state != ST_FORMATTING) {
        du_draw_btn(d, cx, cy, cw, ch, SLOT_LEFT);
        du_draw_btn(d, cx, cy, cw, ch, SLOT_RIGHT);
    }
}

/* ---------------- 动作 ---------------- */

static void du_back_to_main(struct disk_util *d) {
    d->state    = ST_MAIN;
    d->progress = 0;
    d->result   = FS_OK;
    d->init_mode = false;
    d->hbtn     = SLOT_NONE;
    d->hpart    = -1;
    d->focus    = SLOT_LEFT;
    du_build_rows(d);
    du_update_info(d);
    gui_damage();
}

/* 左/右按钮被“按下”后的行为——鼠标点击和键盘 Enter 共用这一条路径 */
static void du_activate(struct disk_util *d, struct gui_window *win, int slot) {
    if (slot == SLOT_NONE || du_busy) return;

    switch (d->state) {
    case ST_MAIN:
        if (slot == SLOT_LEFT) {
            if (!du_btn_enabled(d, SLOT_LEFT)) return;
            const struct du_row *r = du_sel_row(d);
            d->init_mode = (r && r->part_idx < 0);
            d->state = ST_CONFIRM;
            d->hbtn  = SLOT_NONE;
            d->hpart = -1;
            d->focus = SLOT_RIGHT;         /* 危险页默认焦点落在 Cancel */
            gui_damage();
        } else {
            gui_window_close(win);
        }
        break;

    case ST_CONFIRM:
        if (slot == SLOT_LEFT) {
            if (!du_btn_enabled(d, SLOT_LEFT)) return;

            const struct du_row *row = du_sel_row(d);
            if (d->init_mode) {
                /* 先记下目标；du_run_init() 会写 MBR，再格式化 LBA 2048 */
                d->fmt_disk       = row ? row->disk_idx : 0;
                d->fmt_bdev       = -1;
                d->fmt_index      = 0;
                d->fmt_start      = PART_DEFAULT_START;
                d->fmt_was_active = false;
            } else {
                const struct partition_info *p = du_sel_part(d);
                if (!p) return;
                const struct block_device *b =
                    blkdev_find_part(p->disk_idx, p->start_lba);
                d->fmt_disk       = p->disk_idx;
                d->fmt_bdev       = blkdev_index_of(b);
                d->fmt_index      = p->index;
                d->fmt_start      = p->start_lba;
                d->fmt_was_active = du_is_active(p);
            }

            d->state    = ST_FORMATTING;
            d->progress = 0;
            d->result   = FS_OK;
            d->hbtn     = SLOT_NONE;
            d->hpart    = -1;
            gui_damage();
            /* 关键：本回调在 IRQ1 上下文，长任务交给主循环执行 */
            if (d->init_mode) {
                if (!gui_post_action(du_run_init))
                    du_back_to_main(d);
            } else {
                if (!gui_post_action(du_run_format))
                    du_back_to_main(d);
            }
        } else {
            d->state = ST_MAIN;
            d->focus = SLOT_LEFT;
            gui_damage();
        }
        break;

    case ST_DONE:
        if (slot == SLOT_LEFT) gui_window_close(win);
        else                   power_reboot();
        break;

    default:
        break;
    }
}

/* ---------------- 格式化（主循环上下文） ---------------- */

static void du_repaint(struct disk_util *d) {
    if (d->win) {
        gui_damage();
        gui_repaint();          /* 同步刷一帧，只能在主循环上下文调用 */
    } else {
        gui_damage();           /* 窗口中途被关掉：只标脏，交给主循环 */
    }
}

/* 格式化进度回调：由 fs_format_bdev() 每写一个元数据块同步调用一次。
   调用点是 du_run_format() 里的 fs_format_bdev()，即桌面主循环上下文
   （不是 IRQ），所以这里直接重绘是安全的。 */
static void du_progress(int percent, void *user) {
    struct disk_util *d = (struct disk_util *)user;
    if (!d) return;
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    if (percent == d->progress) return;   /* 同一百分比不重复刷 */
    d->progress = percent;
    du_repaint(d);
}

/* 由 gui_post_action() 投递，运行在 gui_run() 主循环里（中断已开启） */
static void du_run_format(void) {
    struct disk_util *d = &du;
    if (d->state != ST_FORMATTING || du_busy) return;
    du_busy = true;

    /* 先把 "正在格式化..." 界面推上屏，进度从 0 起 */
    d->progress = 0;
    du_repaint(d);

    /* 真正格式化：只写 d->fmt_start 指向的分区。
       mkfs 每写一个元数据块 -> du_progress() -> 进度条；FLUSH 后报 100%。
       非活动分区格式化不会让当前 SRFS 失效。 */
    d->result = fs_format_bdev(d->fmt_bdev, du_progress, d);

    d->progress = 100;
    d->state    = ST_DONE;
    d->focus    = SLOT_LEFT;          /* 默认落在 Close，避免误触 Reboot */
    partition_scan();
    blkdev_refresh();
    du_build_rows(d);
    du_update_info(d);

    /* 根分区被格式化后内容作废；其它卷会由 fs_format_bdev 重新挂载。 */
    if (d->result == FS_OK)
        files_refresh_all();

    du_repaint(d);
    du_busy = false;
}

/* 空白盘初始化：写 LBA 0 的 MBR（一个 0x53 分区），再格式化 LBA 2048。
   由 gui_post_action() 投递，运行在桌面主循环上下文。 */
static void du_run_init(void) {
    struct disk_util *d = &du;
    if (d->state != ST_FORMATTING || du_busy) return;
    du_busy = true;

    d->mounted  = false;
    d->progress = 0;
    du_repaint(d);

    d->result = FS_ERR_IO;
    if (partition_create_mbr(d->fmt_disk, PART_DEFAULT_START, PART_TYPE_SRFS)) {
        blkdev_refresh();
        const struct partition_info *p =
            partition_by_disk_start(d->fmt_disk, PART_DEFAULT_START);
        const struct block_device *b =
            blkdev_find_part(d->fmt_disk, PART_DEFAULT_START);
        if (p && b) {
            d->fmt_index      = p->index;
            d->fmt_start      = p->start_lba;
            d->fmt_bdev       = blkdev_index_of(b);
            d->fmt_was_active = false;
            d->result = fs_format_bdev(d->fmt_bdev, du_progress, d);
        }
    }

    /* fs_format_bdev() 对非活动卷会自动重新挂载所有分区；新盘也能立即浏览。 */
    if (d->result == FS_OK) {
        partition_scan();
        blkdev_refresh();
        d->mounted = fs_ready();
        files_refresh_all();
    }

    d->progress = 100;
    d->state    = ST_DONE;
    d->focus    = SLOT_LEFT;          /* 默认落在 Close，避免误触 Reboot */
    du_build_rows(d);
    du_update_info(d);
    du_repaint(d);
    du_busy = false;
}

static void du_click(struct gui_window *win, int32_t cx, int32_t cy, bool dbl) {
    struct disk_util *d = win->data;
    (void)dbl;
    if (!d || du_busy) return;

    if (d->state == ST_MAIN) {
        int row = du_hit_row(gui_content_w(win), gui_content_h(win), cx, cy);
        if (row >= 0) {
            if (d->sel != row) {
                d->sel   = row;
                d->focus = SLOT_LEFT;
                gui_damage();
            }
            return;
        }
    }

    int slot = du_hit_slot(gui_content_w(win), gui_content_h(win),
                           d, cx, cy);
    du_activate(d, win, slot);
}

static void du_hover(struct gui_window *win, int32_t cx, int32_t cy) {
    struct disk_util *d = win->data;
    if (!d) return;

    uint32_t cw = gui_content_w(win), ch = gui_content_h(win);
    int row = -1, slot = SLOT_NONE;

    if (!du_busy && cx >= 0 && cy >= 0) {
        if (d->state == ST_MAIN) row = du_hit_row(cw, ch, cx, cy);
        if (d->state != ST_FORMATTING)
            slot = du_hit_slot(cw, ch, d, cx, cy);
    }

    if (row != d->hpart) {
        if (d->hpart >= 0 && d->hpart >= d->scroll &&
            d->hpart < d->scroll + DU_TBL_ROWS) {
            int32_t rx, ry;
            uint32_t rw, rh;
            du_row_rect(cw, d->hpart - d->scroll, &rx, &ry, &rw, &rh);
            gui_damage_rect(gui_content_x(win) + rx - 1,
                            gui_content_y(win) + ry - 1, rw + 2, rh + 2);
        }
        d->hpart = row;
        if (row >= 0 && row >= d->scroll && row < d->scroll + DU_TBL_ROWS) {
            int32_t rx, ry;
            uint32_t rw, rh;
            du_row_rect(cw, row - d->scroll, &rx, &ry, &rw, &rh);
            gui_damage_rect(gui_content_x(win) + rx - 1,
                            gui_content_y(win) + ry - 1, rw + 2, rh + 2);
        }
    }

    if (slot != d->hbtn) {
        int32_t bx, by;
        uint32_t bw, bh;
        if (d->hbtn != SLOT_NONE) {
            du_btn_rect(cw, ch, d->hbtn, &bx, &by, &bw, &bh);
            gui_damage_rect(gui_content_x(win) + bx - 1,
                            gui_content_y(win) + by - 1, bw + 2, bh + 2);
        }
        d->hbtn = slot;
        if (slot != SLOT_NONE) {
            du_btn_rect(cw, ch, slot, &bx, &by, &bw, &bh);
            gui_damage_rect(gui_content_x(win) + bx - 1,
                            gui_content_y(win) + by - 1, bw + 2, bh + 2);
        }
    }
}

static void du_key(struct gui_window *win, uint16_t key) {
    struct disk_util *d = win->data;
    if (!d || du_busy) return;

    switch (key) {
    case KEY_ESC:
        if (d->state == ST_CONFIRM) {
            d->state = ST_MAIN;            /* 确认页：Esc 只是退回主界面 */
            d->focus = SLOT_LEFT;
            gui_damage();
        } else {
            gui_window_close(win);
        }
        return;

    case KEY_UP:
    case KEY_DOWN:
        if (d->state != ST_MAIN) return;
        {
            int n = d->row_n;
            if (n <= 0) return;
            if (d->sel < 0) d->sel = (key == KEY_DOWN) ? 0 : n - 1;
            else if (key == KEY_DOWN) d->sel = (d->sel + 1) % n;
            else d->sel = (d->sel + n - 1) % n;
            if (d->sel < d->scroll)
                d->scroll = d->sel;
            else if (d->sel >= d->scroll + DU_TBL_ROWS)
                d->scroll = d->sel - DU_TBL_ROWS + 1;
            d->focus = SLOT_LEFT;
            gui_damage();
        }
        return;

    case KEY_LEFT:
        d->focus = SLOT_LEFT;
        gui_damage();
        return;

    case KEY_RIGHT:
        d->focus = SLOT_RIGHT;
        gui_damage();
        return;

    case '\n':
    case ' ':
        du_activate(d, win, d->focus);
        return;

    case 'y':
    case 'Y':
        if (d->state == ST_CONFIRM) du_activate(d, win, SLOT_LEFT);
        return;

    default:
        return;
    }
}

static void du_close(struct gui_window *win) {
    struct disk_util *d = win->data;
    if (d) d->win = 0;                      /* 静态实例保留，下次打开复用 */
}

/* ---------------- 对外接口 ---------------- */

void disk_utility_open(void) {
    struct disk_util *d = &du;

    /* 已经开着：聚焦并回到主界面（正在格式化时不打扰） */
    if (d->win) {
        gui_window_focus(d->win);
        if (!du_busy) du_back_to_main(d);
        return;
    }

    /* 启动时已经扫过；没有磁盘/有效 MBR 时再试一次（例如刚插入磁盘） */
    if (partition_disk_count() == 0 || !partition_valid()) partition_scan();

    memset(d, 0, sizeof(*d));
    d->hbtn   = SLOT_NONE;
    d->hpart  = -1;
    d->focus  = SLOT_LEFT;
    d->sel    = -1;
    d->scroll = 0;
    d->result = FS_OK;
    du_build_rows(d);

    /* 默认选中第一个非活动分区；否则选第一条盘/分区行。 */
    for (int i = 0; i < d->row_n; i++) {
        if (d->rows[i].part_idx < 0) continue;
        if (!du_is_active(partition_get(d->rows[i].part_idx))) {
            d->sel = i;
            break;
        }
    }
    if (d->sel < 0 && d->row_n > 0) d->sel = 0;
    du_build_rows(d);
    du_update_info(d);

    struct gui_window *win = gui_window_open("磁盘工具",
                                             DU_X, DU_Y, DU_W, DU_H);
    if (!win) return;

    d->win        = win;
    win->data     = d;
    win->draw     = du_draw;
    win->on_click = du_click;
    win->on_hover = du_hover;
    win->on_key   = du_key;
    win->on_close = du_close;
    gui_window_set_icon(win, GUI_ICON_DISK);

    gui_damage();
}
