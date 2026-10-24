#include "files.h"
#include "gfx/gui.h"
#include "gfx/gfx.h"
#include "fs/fs.h"
#include "fs/partition.h"
#include "fs/vfs.h"
#include "apps/terminal.h"
#include "drivers/keyboard.h"
#include "drivers/timer.h"
#include "lib/mem.h"
#include "lib/string.h"

/* ============================================================
 *  资源管理器（“此电脑” This PC）
 *
 *  参照 Windows 11 资源管理器布局：
 *    [命令栏]  New folder / Delete / Refresh
 *    [地址栏]  < > ^  面包屑路径
 *    +----------------+----------------------------------+
 *    | 导航窗格        | Folders (n)  大图标网格           |
 *    |  Quick access  |                                  |
 *    |  This PC       | Devices and drives (1)  容量条    |
 *    +----------------+----------------------------------+
 *    [状态栏]  N items                    X MB free of Y MB
 *
 *  双击目录进入、双击文件查看；Del 删除、N 新建文件夹、R 刷新、
 *  Backspace 返回上级、方向键在网格中移动选择。
 *  文本查看视图：可滚动，Esc/Backspace 返回。
 *
 *  约束：与其它应用一致——不使用 malloc，全部为文件级静态实例。
 *  交互回调运行在 PS/2 中断上下文，只做状态变更 + gui_damage_*，
 *  耗时的磁盘操作（新建/删除）都很短，直接同步执行。
 *
 *  所有布局坐标分两类：
 *    - 绘制用的“内容区相对坐标”（下面各布局函数算出来的 dx/dy）
 *    - 命中测试用的 cx/cy（on_click/on_hover 传入，本身就是内容区
 *      相对坐标），两者可直接比较。
 * ============================================================ */

#define FM_MAX      6
#define FM_W        860
#define FM_H        540
#define FM_X        150
#define FM_Y        96

#define CMD_H       30          /* 命令栏高 */
#define ADDR_H      28          /* 地址栏高 */
#define NAV_W       168         /* 左侧导航窗格宽 */
#define SEC_H       22          /* 分组标题高 */
#define STATUS_H    22
#define TILE_W      210         /* 大图标网格单元 */
#define TILE_H      58
#define DRIVE_H     58          /* 驱动器行高 */
#define VIEW_MAX    8192        /* 文本查看器最多读入的字节数 */

#define SIDE_MAX    12
#define HIST_MAX    8
#define SIDE_HDR_H  22
#define SIDE_ROW_H  22
#define SIDE_GAP    4

/* 文件拖放 / 复制粘贴 */
#define FM_XFER_MAX      FS_MAX_DIRENTS
#define FM_WALK_DEPTH    6       /* 目录递归深度上限（静态数组，无堆） */
#define FM_COPY_CHUNK    8192
#define FM_STATUS_TICKS  400     /* 状态栏消息保留约 4 秒 */

/* 新建/重命名输入模式 */
enum { FM_PROMPT_NEW = 0, FM_PROMPT_RENAME = 1 };

/* 命令栏按钮 */
#define CB_N 3
enum { CB_NEW = 0, CB_DEL, CB_REFRESH };
static const char *const cb_label[CB_N] = { "新建文件夹", "删除", "刷新" };
static const uint32_t    cb_w[CB_N]     = { 96, 70, 78 };
#define CB_DY 4
#define CB_H  (CMD_H - 8)

/* 地址栏箭头 */
#define AB_N 3
enum { AB_BACK = 0, AB_FWD, AB_UP };
#define ABTN_SZ 22

/* 导航窗格条目类型 */
#define SIDE_FOLDER 0
#define SIDE_DRIVE  1
#define SIDE_USB    2

/* 属性对话框（窗口内小型模态框） */
#define PROPS_W     380
#define PROPS_H     216
#define PROPS_BTN_W 76
#define PROPS_BTN_H 24

/* 文件夹右下角徽标 */
enum { BD_NONE = 0, BD_DESKTOP, BD_DOWNLOAD, BD_DOC, BD_IMAGE, BD_MUSIC, BD_VIDEO };

/* 悬停分区 */
enum { HS_NONE = 0, HS_FOLDERS, HS_DRIVES };

struct side_item {
    char    label[40];
    char    path[FS_PATH_MAX];
    uint8_t kind;
    uint8_t group;                 /* 0 = Quick access, 1 = This PC */
};

struct fm {
    struct gui_window *win;
    char     path[FS_PATH_MAX];

    /* 历史：后退 / 前进 两个栈 */
    char     back[HIST_MAX][FS_PATH_MAX];
    int      back_n;
    char     fwd[HIST_MAX][FS_PATH_MAX];
    int      fwd_n;

    /* 目录内容 */
    struct fs_dirent ents[FS_MAX_DIRENTS];
    int      n;
    int      sel;
    int      top;
    bool     selected[FS_MAX_DIRENTS];   /* Ctrl 多选 / 框选 */

    /* 框选（内容区坐标，网格范围内） */
    bool     drag_active;
    bool     drag_ctrl;
    bool     drag_moved;
    int32_t  drag_x0, drag_y0, drag_x1, drag_y1;

    /* 拖动文件到文件夹（内容区坐标） */
    bool     file_drag;
    int      press_idx;            /* 按下时命中的网格项，-1 无 */
    int32_t  press_x, press_y;
    int32_t  drag_cx, drag_cy;
    bool     drop_valid;
    char     drop_path[FS_PATH_MAX];

    /* 导航窗格 */
    struct side_item side[SIDE_MAX];
    int      side_n;

    /* VFS 挂载表快照（This PC 的驱动器列表） */
    struct vfs_mount_info mounts[VFS_MAX_MOUNTS];
    int      mount_n;

    /* 分组折叠 */
    bool     folders_open;
    bool     drives_open;

    /* 悬停状态 */
    int      hcmd;                 /* 命令按钮，-1 无 */
    int      hab;                  /* 地址栏箭头，-1 无 */
    int      hside;                /* 导航项，-1 无 */
    int      htile;                /* 网格项，-1 无 */
    int      hsec;                 /* HS_* */
    int      hdrive;               /* 驱动器行下标，-1 无 */

    /* 文本查看模式 */
    bool     viewing;
    char     file[FS_PATH_MAX];
    uint64_t fsize;
    uint8_t  vbuf[VIEW_MAX];
    int      vlen;
    uint64_t vscroll;

    /* 操作状态：新建/重命名输入 / 删除确认 */
    bool     prompt;
    int      prompt_action;        /* FM_PROMPT_NEW / FM_PROMPT_RENAME */
    int      prompt_type;          /* FS_TYPE_DIR / FS_TYPE_FILE */
    int      rename_idx;
    char     pbuf[FS_NAME_MAX + 1];
    int      plen;
    int      confirm_del;
    int      del_count;

    /* 属性对话框 */
    bool     props_open;
    int      props_idx;
    struct fs_dirent props_st;
    char     props_path[FS_PATH_MAX];

    /* 右键菜单状态：0 无 / 1 网格项 / 2 空白处 */
    int      ctx_mode;
    int      ctx_tile;
};

/* 主区布局（内容区相对坐标） */
struct layout {
    int32_t  mx, my;
    uint32_t mw, mh;
    int32_t  sec_y;                /* Folders 分组标题 */
    int32_t  grid_y;
    uint32_t grid_h;
    int32_t  drv_y;                /* Devices and drives 分组标题，无则 0 */
    uint32_t drv_h;
    int      cols, rows, visible;
};

static struct fm fms[FM_MAX];

/* ---------------- 剪贴板 / 文件传输 ----------------
   剪贴板是跨窗口共享的单例；路径快照在粘贴时使用，避免用户在操作过程中
   导航到别处后误用。真正的磁盘复制/移动投递到 gui_run() 主循环执行。 */
struct fm_clip_item {
    char    path[FS_PATH_MAX];
    uint8_t type;
};

static struct fm_clip_item g_clip[FM_XFER_MAX];
static int                g_clip_n;
static bool               g_clip_cut;

struct fm_xfer {
    bool     move;
    bool     from_clipboard;
    int      n;
    char     dest[FS_PATH_MAX];
    struct fm_clip_item items[FM_XFER_MAX];
};

static struct fm_xfer g_xfer;
static volatile int    g_fs_busy;
static struct fm_clip_item g_del_items[FM_XFER_MAX];
static volatile int    g_del_n;
static uint8_t         g_copy_buf[FM_COPY_CHUNK];
static struct fs_dirent g_walk_ents[FM_WALK_DEPTH][FS_MAX_DIRENTS];

static char     g_status[FS_PATH_MAX];
static uint64_t g_status_tick;

static void layout_calc(struct gui_window *win, struct fm *fm, struct layout *L);
static bool fm_drop_is(struct fm *fm, const char *path);
static bool fm_item_is_cut(struct fm *fm, int idx);
static void fm_draw_drag_ghost(struct fm *fm, int32_t ox, int32_t oy);
static void fm_xfer_run(void);
static void fm_delete_run(void);

/* ---------------- 小工具 ---------------- */
static void str_app(char *dst, const char *src) {
    kstrcpy(dst + kstrlen(dst), src);
}

static bool has_word_ci(const char *s, const char *w) {
    for (const char *p = s; *p; p++) {
        const char *a = p, *b = w;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
            if (ca != cb) break;
            a++; b++;
        }
        if (!*b) return true;
    }
    return false;
}

static int folder_badge(const char *name) {
    if (has_word_ci(name, "desktop"))  return BD_DESKTOP;
    if (has_word_ci(name, "download")) return BD_DOWNLOAD;
    if (has_word_ci(name, "doc"))      return BD_DOC;
    if (has_word_ci(name, "picture") ||
        has_word_ci(name, "image"))    return BD_IMAGE;
    if (has_word_ci(name, "music") ||
        has_word_ci(name, "audio"))    return BD_MUSIC;
    if (has_word_ci(name, "video") ||
        has_word_ci(name, "movie"))    return BD_VIDEO;
    return BD_NONE;
}

/* ---------------- 路径工具 ---------------- */
static void path_join(char *out, const char *dir, const char *name) {
    size_t n = 0;
    for (size_t i = 0; dir[i] && n + 1 < FS_PATH_MAX; i++) out[n++] = dir[i];
    if (n == 0 || out[n - 1] != '/') {
        if (n + 1 < FS_PATH_MAX) out[n++] = '/';
    }
    for (size_t i = 0; name[i] && n + 1 < FS_PATH_MAX; i++) out[n++] = name[i];
    out[n] = 0;
}

static void path_up(char *out, const char *path) {
    char tmp[FS_PATH_MAX];
    kstrcpy(tmp, path);
    size_t l = kstrlen(tmp);
    while (l > 1 && tmp[l - 1] == '/') tmp[--l] = 0;
    if (l <= 1) { kstrcpy(out, "/"); return; }
    while (l > 1 && tmp[l - 1] != '/') l--;
    if (l <= 1) { kstrcpy(out, "/"); return; }
    tmp[l - 1] = 0;
    kstrcpy(out, tmp);
}

static const char *path_base(const char *path) {
    if (!path || !path[0] || !kstrcmp(path, "/")) return "此电脑";
    const char *b = path;
    for (const char *s = path; *s; s++)
        if (*s == '/' && s[1]) b = s + 1;
    return b;
}

static bool path_is_root(const char *path) {
    return !path || !path[0] || !kstrcmp(path, "/");
}

/* 有界路径拷贝：外部路径最多 FS_PATH_MAX-1 字节。 */
static void path_copy(char *dst, const char *src) {
    size_t n = 0;
    if (!src) { dst[0] = 0; return; }
    while (src[n] && n + 1 < FS_PATH_MAX) { dst[n] = src[n]; n++; }
    dst[n] = 0;
}

/* 路径最后一段（根路径返回空串）。 */
static void path_leaf(const char *path, char *out) {
    const char *b = path ? path : "";
    for (const char *s = b; *s; s++)
        if (*s == '/' && s[1]) b = s + 1;
    path_copy(out, b);
}

static size_t path_trim_len(const char *path) {
    size_t n = kstrlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    return n;
}

/* path 等于 dir，或位于 dir 之下。 */
static bool path_within_or_same(const char *path, const char *dir) {
    size_t dl = path_trim_len(dir);
    size_t pl = path_trim_len(path);
    if (dl == 1 && dir[0] == '/') return true;
    if (pl < dl) return false;
    for (size_t i = 0; i < dl; i++)
        if (path[i] != dir[i]) return false;
    return (pl == dl) || (path[dl] == '/');
}

/* 所有挂载为 SRFS 的卷都支持 create/remove/rename；只读文件系统除外。 */
static bool path_on_srfs(const char *path) {
    if (!path) return false;
    size_t ml = 0;
    const struct filesystem_ops *ops = vfs_route(path, 0, &ml);
    (void)ml;
    return ops && ops->name && !kstrcmp(ops->name, "srfs");
}

static void fm_status(const char *msg) {
    path_copy(g_status, msg);
    g_status_tick = timer_ticks();
    gui_damage();
}

/* ---------------- 多选状态 ---------------- */
static void fm_clear_selection(struct fm *fm) {
    for (int i = 0; i < FS_MAX_DIRENTS; i++) fm->selected[i] = false;
}

static int fm_sel_count(struct fm *fm) {
    int c = 0;
    for (int i = 0; i < fm->n && i < FS_MAX_DIRENTS; i++)
        if (fm->selected[i]) c++;
    return c;
}

static void fm_select_only(struct fm *fm, int idx) {
    fm_clear_selection(fm);
    if (idx >= 0 && idx < fm->n && idx < FS_MAX_DIRENTS)
        fm->selected[idx] = true;
    fm->sel = (idx >= 0 && idx < fm->n) ? idx : -1;
}

static void fm_toggle_selected(struct fm *fm, int idx) {
    if (idx < 0 || idx >= fm->n || idx >= FS_MAX_DIRENTS) return;
    fm->selected[idx] = !fm->selected[idx];
    fm->sel = idx;
}

static void fm_select_all(struct fm *fm) {
    for (int i = 0; i < fm->n; i++)
        if (i < FS_MAX_DIRENTS) fm->selected[i] = true;
    fm->sel = fm->n ? 0 : -1;
}

static bool fm_item_is_cut(struct fm *fm, int idx) {
    if (!g_clip_cut || g_clip_n <= 0 || idx < 0 || idx >= fm->n)
        return false;
    char p[FS_PATH_MAX];
    path_join(p, fm->path, fm->ents[idx].name);
    for (int i = 0; i < g_clip_n; i++)
        if (!kstrcmp(p, g_clip[i].path)) return true;
    return false;
}

static bool fm_drop_is(struct fm *fm, const char *path) {
    return fm->file_drag && fm->drop_valid && !kstrcmp(fm->drop_path, path);
}

/* 属性框里的修改时间：0 表示旧 inode / RTC 不可用。 */
static void put_num_pad(char *dst, int v, int width) {
    for (int i = width - 1; i >= 0; i--) {
        dst[i] = (char)('0' + (v % 10));
        v /= 10;
    }
}

static void fmt_mtime(uint32_t t, char *out) {
    if (!t) { kstrcpy(out, "N/A"); return; }
    int year = 2000 + (int)((t >> 26) & 0x3F);
    int mon  = (int)((t >> 22) & 0x0F);
    int day  = (int)((t >> 17) & 0x1F);
    int hour = (int)((t >> 12) & 0x1F);
    int min  = (int)((t >> 6) & 0x3F);
    int sec  = (int)(t & 0x3F);
    put_num_pad(out, year, 4);       out[4] = '-';
    put_num_pad(out + 5, mon, 2);    out[7] = '-';
    put_num_pad(out + 8, day, 2);    out[10] = ' ';
    put_num_pad(out + 11, hour, 2);  out[13] = ':';
    put_num_pad(out + 14, min, 2);   out[16] = ':';
    put_num_pad(out + 17, sec, 2);   out[19] = 0;
}

/* ---------------- 排序 / 格式化 ---------------- */
static int name_cmp_ci(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return (unsigned char)ca - (unsigned char)cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* 目录在前，其次按名字不区分大小写排序（插入排序，n 很小） */
static void sort_ents(struct fm *fm) {
    for (int i = 1; i < fm->n; i++) {
        struct fs_dirent key = fm->ents[i];
        int j = i - 1;
        while (j >= 0) {
            struct fs_dirent *e = &fm->ents[j];
            int dir_key = (key.type == FS_TYPE_DIR);
            int dir_e   = (e->type == FS_TYPE_DIR);
            int c = (dir_key != dir_e) ? (dir_key - dir_e)
                                       : name_cmp_ci(e->name, key.name);
            if (c <= 0) break;
            fm->ents[j + 1] = fm->ents[j];
            j--;
        }
        fm->ents[j + 1] = key;
    }
}

static void size_str(uint64_t sz, char *out) {
    char b[24];
    if (sz < 1024) {
        kstrcpy(out, utoa(sz, b, 10));
        str_app(out, " B");
    } else if (sz < 1024 * 1024) {
        kstrcpy(out, utoa(sz / 1024, b, 10));
        str_app(out, " KB");
    } else {
        kstrcpy(out, utoa(sz / 1024 / 1024, b, 10));
        str_app(out, " MB");
    }
}

/* ---------------- 几何（内容区相对坐标） ---------------- */
static int32_t cbtn_dx(int i) {
    int32_t x = 8;
    for (int k = 0; k < i; k++) x += (int32_t)cb_w[k] + 6;
    return x;
}

static int32_t abtn_dx(int i) { return 8 + i * (ABTN_SZ + 4); }
static int32_t abtn_dy(void)  { return CMD_H + (ADDR_H - ABTN_SZ) / 2; }
static int32_t body_dy(void)  { return CMD_H + ADDR_H; }

static uint32_t fm_body_h(struct gui_window *win) {
    uint32_t ch = gui_content_h(win);
    uint32_t used = CMD_H + ADDR_H + STATUS_H;
    return ch > used ? ch - used : 1;
}

static void layout_calc(struct gui_window *win, struct fm *fm, struct layout *L) {
    uint32_t cw = gui_content_w(win);
    L->mx = NAV_W + 8;
    L->my = body_dy();
    L->mw = cw > NAV_W + 16 ? cw - NAV_W - 16 : 16;
    L->mh = fm_body_h(win);
    L->sec_y = L->my;
    L->grid_y = L->my + SEC_H;
    L->drv_y = 0;
    L->drv_h = 0;
    L->grid_h = 0;

    L->cols = (int)(L->mw / TILE_W);
    if (L->cols < 1) L->cols = 1;

    bool root = path_is_root(fm->path) && fs_ready();
    /* 分组标题自身占 SEC_H，之后才是可用高度 */
    uint32_t after_sec = L->mh > SEC_H ? L->mh - SEC_H : 0;

    if (root && fm->drives_open && after_sec > SEC_H + DRIVE_H + 2 * TILE_H) {
        int dn = fm->mount_n > 0 ? fm->mount_n : 1;
        if (dn > 5) dn = 5;                 /* 侧边栏仍可访问其余卷 */
        uint32_t want = (uint32_t)dn * DRIVE_H;
        uint32_t maxh = after_sec - SEC_H - 2 * TILE_H;
        if (want > maxh) want = maxh - (maxh % DRIVE_H);
        if (want >= DRIVE_H) L->drv_h = want;
    }

    if (root) {
        /* 驱动器组自身还要占一个分组标题的高度 */
        uint32_t drv_zone = L->drv_h ? (SEC_H + L->drv_h) : 0;
        uint32_t grid_avail = after_sec > drv_zone ? after_sec - drv_zone : 0;
        L->grid_h = fm->folders_open ? grid_avail : 0;
        L->drv_y  = L->my + (int32_t)(L->grid_h + SEC_H);
    } else {
        L->grid_h = fm->folders_open ? after_sec : 0;
    }

    L->rows = (int)(L->grid_h / TILE_H);
    if (L->rows < 1) L->rows = 1;
    L->visible = L->cols * L->rows;
}

static bool tile_dx(struct fm *fm, struct layout *L, int idx,
                    int32_t *tx, int32_t *ty) {
    if (idx < fm->top || L->grid_h == 0) return false;
    int i = idx - fm->top;
    if (i >= L->visible) return false;
    *tx = L->mx + (i % L->cols) * (int32_t)TILE_W;
    *ty = L->grid_y + (i / L->cols) * (int32_t)TILE_H;
    return true;
}

static void fm_ensure_visible(struct fm *fm, struct layout *L) {
    if (fm->sel < 0) fm->sel = 0;
    if (fm->sel >= fm->n) fm->sel = fm->n ? fm->n - 1 : 0;
    int cols = L->cols > 0 ? L->cols : 1;
    int vis  = L->visible > 0 ? L->visible : 1;
    if (fm->sel < fm->top) fm->top = (fm->sel / cols) * cols;
    if (fm->sel >= fm->top + vis) {
        fm->top = (fm->sel / cols - L->rows + 1) * cols;
        if (fm->top < 0) fm->top = 0;
    }
    if (fm->top < 0) fm->top = 0;
}

/* 导航窗格某条目相对内容区的 y（与 draw_sidebar 的顺序严格一致） */
static int32_t side_dy(struct fm *fm, int idx) {
    int32_t y = body_dy() + 4;
    for (int g = 0; g < 2; g++) {
        y += SIDE_HDR_H;
        for (int i = 0; i < fm->side_n; i++) {
            if (fm->side[i].group != (uint8_t)g) continue;
            if (i == idx) return y;
            y += SIDE_ROW_H;
        }
        y += SIDE_GAP;
    }
    return -1;
}

/* ---------------- 数据装载 / 导航 ---------------- */
static void fm_load(struct fm *fm) {
    fm->n = 0;
    fm->htile = -1;
    fm->hdrive = -1;
    fm->confirm_del = -1;
    fm->del_count = 0;
    fm->press_idx = -1;
    fm->file_drag = false;
    fm->drop_valid = false;
    fm_clear_selection(fm);
    fm->sel = -1;
    if (!fs_ready()) return;
    int n = fs_listdir(fm->path, fm->ents, FS_MAX_DIRENTS);
    if (n < 0) n = 0;
    fm->n = n;
    sort_ents(fm);
    if (fm->n) {
        fm->sel = 0;
        fm->selected[0] = true;
    }
    fm->top = 0;
}

static void mount_label(const struct vfs_mount_info *m, char *out, size_t cap) {
    if (m->is_root) {
        size_t n = 0;
        const char *s = "StarRing OS";
        while (*s && n + 1 < cap) out[n++] = *s++;
        out[n] = 0;
        return;
    }
    size_t n = 0;
    for (const char *s = m->source; *s && n + 1 < cap; s++) out[n++] = *s;
    if (n + 3 < cap) {
        out[n++] = ' ';
        out[n++] = '(';
        for (const char *s = m->fs_name; *s && n + 2 < cap; s++) out[n++] = *s;
        out[n++] = ')';
    }
    out[n] = 0;
}

static void fm_build_side(struct fm *fm) {
    static const struct { const char *label; const char *path; } q[] = {
        { "桌面",    "/home/guest/Desktop"   },
        { "下载",    "/home/guest/Downloads" },
        { "文档",    "/home/guest/Documents" },
        { "图片",    "/home/guest/Pictures"  },
        { "音乐",    "/home/guest/Music"     },
    };
    fm->side_n = 0;
    fm->mount_n = vfs_list_mounts(fm->mounts, VFS_MAX_MOUNTS);
    if (!fs_ready() && fm->mount_n == 0) return;

    for (size_t i = 0; i < sizeof(q) / sizeof(q[0]) && fm->side_n < SIDE_MAX; i++) {
        if (!fs_exists(q[i].path)) continue;
        struct side_item *s = &fm->side[fm->side_n++];
        kstrcpy(s->label, q[i].label);
        kstrcpy(s->path, q[i].path);
        s->kind = SIDE_FOLDER;
        s->group = 0;
    }

    /* 动态列出 VFS 挂载表中的所有卷。 */
    for (int i = 0; i < fm->mount_n && fm->side_n < SIDE_MAX; i++) {
        struct side_item *s = &fm->side[fm->side_n++];
        mount_label(&fm->mounts[i], s->label, sizeof(s->label));
        kstrcpy(s->path, fm->mounts[i].path);
        bool usb = (fm->mounts[i].path[0] == '/' &&
                    fm->mounts[i].path[1] == 'm' &&
                    fm->mounts[i].path[2] == 'n' &&
                    fm->mounts[i].path[3] == 't' &&
                    fm->mounts[i].path[4] == '/' &&
                    fm->mounts[i].path[5] == 'u' &&
                    fm->mounts[i].path[6] == 's' &&
                    fm->mounts[i].path[7] == 'b');
        s->kind = usb ? SIDE_USB : SIDE_DRIVE;
        s->group = 1;
    }
}

/* 真正落地到某个路径（不入历史） */
static void fm_goto(struct fm *fm, const char *path) {
    kstrcpy(fm->path, path);
    fm->viewing = false;
    fm->prompt  = false;
    fm->confirm_del = -1;
    fm->sel = 0;
    fm->top = 0;
    fm_load(fm);
    if (fm->win) gui_window_set_title(fm->win, path_base(fm->path));
    gui_damage();
}

static void hist_push(char hist[][FS_PATH_MAX], int *n, const char *p) {
    if (*n >= HIST_MAX) {
        for (int i = 1; i < HIST_MAX; i++) kstrcpy(hist[i - 1], hist[i]);
        *n = HIST_MAX - 1;
    }
    kstrcpy(hist[(*n)++], p);
}

/* push=true 表示“进行新的导航”（写入后退栈，清空前进栈） */
static void fm_nav(struct fm *fm, const char *path, bool push) {
    if (push) {
        if (kstrcmp(path, fm->path) == 0) { fm_goto(fm, path); return; }
        char cur[FS_PATH_MAX];
        kstrcpy(cur, fm->path);
        hist_push(fm->back, &fm->back_n, cur);
        fm->fwd_n = 0;
    }
    fm_goto(fm, path);
}

static void fm_back(struct fm *fm) {
    if (fm->back_n <= 0) return;
    char dest[FS_PATH_MAX], cur[FS_PATH_MAX];
    kstrcpy(dest, fm->back[--fm->back_n]);
    kstrcpy(cur, fm->path);
    hist_push(fm->fwd, &fm->fwd_n, cur);
    fm_goto(fm, dest);
}

static void fm_fwd(struct fm *fm) {
    if (fm->fwd_n <= 0) return;
    char dest[FS_PATH_MAX], cur[FS_PATH_MAX];
    kstrcpy(dest, fm->fwd[--fm->fwd_n]);
    kstrcpy(cur, fm->path);
    hist_push(fm->back, &fm->back_n, cur);
    fm_goto(fm, dest);
}

static void fm_up(struct fm *fm) {
    if (path_is_root(fm->path)) return;
    char up[FS_PATH_MAX];
    path_up(up, fm->path);
    fm_nav(fm, up, true);
}

static void fm_open_file(struct fm *fm, const char *path) {
    kstrcpy(fm->file, path);
    fm->viewing = true;
    fm->prompt  = false;
    fm->vscroll = 0;
    fm->vlen = 0;
    fm->htile = -1;
    if (fs_ready()) {
        int r = fs_read(path, fm->vbuf, VIEW_MAX - 1, 0);
        if (r > 0) fm->vlen = r;
    }
    fm->vbuf[fm->vlen] = 0;
    struct fs_dirent st;
    fm->fsize = (fs_stat(path, &st) == FS_OK) ? st.size : (uint64_t)fm->vlen;
    if (fm->win) gui_window_set_title(fm->win, path_base(path));
    gui_damage();
}

static void fm_open_selected(struct fm *fm) {
    if (fm->sel < 0 || fm->sel >= fm->n) return;
    struct fs_dirent *e = &fm->ents[fm->sel];
    char p[FS_PATH_MAX];
    path_join(p, fm->path, e->name);
    if (e->type == FS_TYPE_DIR) fm_nav(fm, p, true);
    else                        fm_open_file(fm, p);
}

/* 查看器里文本的总行数（至少 1 行） */
static int viewer_lines(struct fm *fm) {
    int lines = 1;
    for (int i = 0; i < fm->vlen; i++)
        if (fm->vbuf[i] == '\n') lines++;
    return lines;
}

/* ---------------- 复制 / 移动 / 剪贴板 ---------------- */

static void numbered_name(const char *name, int n, char *out) {
    char num[16];
    utoa((uint64_t)n, num, 10);

    size_t nl = kstrlen(name);
    int dot = -1;
    for (size_t i = nl; i > 0; i--) {
        if (name[i - 1] == '.' && i - 1 > 0) { dot = (int)(i - 1); break; }
    }
    size_t blen = (dot >= 0) ? (size_t)dot : nl;
    const char *ext = (dot >= 0) ? name + dot : "";
    size_t elen = (dot >= 0) ? nl - blen : 0;
    size_t slen = kstrlen(num) + 3;                 /* " (n)" */
    if (slen + elen >= FS_NAME_MAX) {               /* 极端长后缀：退化为 copyN */
        size_t p = 0;
        kstrcpy(out, "copy");
        p = 4;
        for (size_t i = 0; num[i] && p + 1 < FS_NAME_MAX; i++)
            out[p++] = num[i];
        out[p] = 0;
        return;
    }
    size_t maxbase = FS_NAME_MAX - 1 - slen - elen;
    if (maxbase > blen) maxbase = blen;

    size_t p = 0;
    for (size_t i = 0; i < maxbase; i++) out[p++] = name[i];
    out[p++] = ' ';
    out[p++] = '(';
    for (size_t i = 0; num[i]; i++) out[p++] = num[i];
    out[p++] = ')';
    for (size_t i = 0; i < elen; i++) out[p++] = ext[i];
    out[p] = 0;

    if (p == 0) {
        kstrcpy(out, "copy");
        p = 4;
        if (p + kstrlen(num) < FS_NAME_MAX)
            kstrcpy(out + p, num);
    }
}

/* 在 dir 中为 name 找一个不冲突的新名字（name、name (2)、name (3)…）。 */
static bool fm_unique_path(const char *dir, const char *name, char *out) {
    path_join(out, dir, name);
    if (!fs_exists(out)) return true;
    for (int i = 2; i < 1000; i++) {
        char cand[FS_NAME_MAX + 1];
        numbered_name(name, i, cand);
        path_join(out, dir, cand);
        if (!fs_exists(out)) return true;
    }
    return false;
}

static int fm_copy_tree(const char *src, const char *dst, int depth) {
    if (depth >= FM_WALK_DEPTH) return FS_ERR_INVAL;
    if (path_is_root(src) || path_is_root(dst)) return FS_ERR_INVAL;
    if (!path_on_srfs(src) || !path_on_srfs(dst)) return FS_ERR_INVAL;

    struct fs_dirent st;
    int r = fs_stat(src, &st);
    if (r < 0) return r;

    if (st.type == FS_TYPE_DIR) {
        r = fs_create(dst, FS_TYPE_DIR);
        if (r < 0) return r;

        struct fs_dirent *ents = g_walk_ents[depth];
        int n = fs_listdir(src, ents, FS_MAX_DIRENTS);
        if (n < 0) return n;
        /* 该 API 无法翻页；达到上限时拒绝复制，避免静默漏文件。 */
        if (n >= FS_MAX_DIRENTS) return FS_ERR_NOSPACE;
        for (int i = 0; i < n; i++) {
            char cs[FS_PATH_MAX], cd[FS_PATH_MAX];
            path_join(cs, src, ents[i].name);
            path_join(cd, dst, ents[i].name);
            r = fm_copy_tree(cs, cd, depth + 1);
            if (r < 0) return r;
        }
        return FS_OK;
    }

    if (st.type != FS_TYPE_FILE) return FS_ERR_INVAL;
    r = fs_create(dst, FS_TYPE_FILE);
    if (r < 0) return r;

    uint64_t off = 0;
    for (;;) {
        int rd = fs_read(src, g_copy_buf, sizeof(g_copy_buf), off);
        if (rd < 0) return rd;
        if (rd == 0) break;
        int wr = fs_write(dst, g_copy_buf, (uint64_t)rd, off);
        if (wr < 0) return wr;
        if (wr != rd) return FS_ERR_IO;
        off += (uint64_t)rd;
    }
    return FS_OK;
}

static int fm_remove_tree(const char *path, int depth) {
    if (depth >= FM_WALK_DEPTH) return FS_ERR_INVAL;
    if (path_is_root(path) || !path_on_srfs(path)) return FS_ERR_INVAL;

    struct fs_dirent st;
    int r = fs_stat(path, &st);
    if (r < 0) return r;

    if (st.type == FS_TYPE_DIR) {
        struct fs_dirent *ents = g_walk_ents[depth];
        int n = fs_listdir(path, ents, FS_MAX_DIRENTS);
        if (n < 0) return n;
        /* 先确认能完整枚举，再开始删除，避免删到一半报错。 */
        if (n >= FS_MAX_DIRENTS) return FS_ERR_NOSPACE;
        for (int i = 0; i < n; i++) {
            char child[FS_PATH_MAX];
            path_join(child, path, ents[i].name);
            r = fm_remove_tree(child, depth + 1);
            if (r < 0) return r;
        }
    }
    return fs_remove(path);
}

/* 删除前先完整检查整棵树，避免删到一半才发现超限或遇到挂载点。 */
static int fm_tree_fits(const char *path, int depth) {
    if (depth >= FM_WALK_DEPTH) return FS_ERR_INVAL;
    if (path_is_root(path) || !path_on_srfs(path)) return FS_ERR_INVAL;

    struct fs_dirent st;
    int r = fs_stat(path, &st);
    if (r < 0) return r;
    if (st.type == FS_TYPE_FILE) return FS_OK;
    if (st.type != FS_TYPE_DIR) return FS_ERR_INVAL;

    struct fs_dirent *ents = g_walk_ents[depth];
    int n = fs_listdir(path, ents, FS_MAX_DIRENTS);
    if (n < 0) return n;
    if (n >= FS_MAX_DIRENTS) return FS_ERR_NOSPACE;
    for (int i = 0; i < n; i++) {
        char child[FS_PATH_MAX];
        path_join(child, path, ents[i].name);
        r = fm_tree_fits(child, depth + 1);
        if (r < 0) return r;
    }
    return FS_OK;
}

static bool fm_selection_srfs(struct fm *fm) {
    if (fm_sel_count(fm) <= 0) return false;
    for (int i = 0; i < fm->n && i < FS_MAX_DIRENTS; i++) {
        if (!fm->selected[i]) continue;
        char p[FS_PATH_MAX];
        path_join(p, fm->path, fm->ents[i].name);
        if (!path_on_srfs(p)) return false;
    }
    return true;
}

static int fm_selection_items(struct fm *fm, struct fm_clip_item *out, int max) {
    int n = 0;
    for (int i = 0; i < fm->n && i < FS_MAX_DIRENTS && n < max; i++) {
        if (!fm->selected[i]) continue;
        path_join(out[n].path, fm->path, fm->ents[i].name);
        out[n].type = fm->ents[i].type;
        n++;
    }
    return n;
}

static bool fm_clip_set(struct fm *fm, bool cut) {
    if (!fm_selection_srfs(fm)) {
        fm_status("复制/剪切仅支持 StarRing OS 卷");
        gui_damage();
        return false;
    }
    int n = fm_selection_items(fm, g_clip, FM_XFER_MAX);
    if (n <= 0) return false;
    g_clip_n = n;
    g_clip_cut = cut;

    char msg[64], b[16];
    kstrcpy(msg, cut ? "剪切 " : "已复制 ");
    str_app(msg, utoa((uint64_t)n, b, 10));
    str_app(msg, n == 1 ? " 项" : " 项");
    fm_status(msg);
    gui_damage();
    return true;
}

static bool fm_xfer_begin(struct fm *fm, const char *dest, bool move,
                          bool from_clipboard) {
    if (g_fs_busy) {
        fm_status("另一个文件操作仍在进行中");
        gui_damage();
        return false;
    }
    if (!dest || !path_on_srfs(dest)) {
        fm_status("粘贴仅支持 StarRing OS 卷");
        gui_damage();
        return false;
    }

    int n = 0;
    if (from_clipboard) {
        if (g_clip_n <= 0) return false;
        n = g_clip_n;
        for (int i = 0; i < n; i++) g_xfer.items[i] = g_clip[i];
    } else {
        n = fm_selection_items(fm, g_xfer.items, FM_XFER_MAX);
    }
    if (n <= 0) return false;

    for (int i = 0; i < n; i++) {
        if (!path_on_srfs(g_xfer.items[i].path)) {
            fm_status("无法从此位置复制");
            gui_damage();
            return false;
        }
        if (path_within_or_same(dest, g_xfer.items[i].path)) {
            fm_status("不能把文件夹放入其自身");
            gui_damage();
            return false;
        }
    }

    g_xfer.move = move;
    g_xfer.from_clipboard = from_clipboard;
    g_xfer.n = n;
    path_copy(g_xfer.dest, dest);
    g_fs_busy = 1;
    if (!gui_post_action(fm_xfer_run)) {
        g_fs_busy = 0;
        fm_status("文件操作队列已满");
        gui_damage();
        return false;
    }

    fm_status(move ? "正在移动..." : "正在复制...");
    gui_damage();
    return true;
}

static void fm_xfer_run(void) {
    int ok = 0, failed = 0, skipped = 0;

    for (int i = 0; i < g_xfer.n; i++) {
        const char *src = g_xfer.items[i].path;
        if (!path_on_srfs(src) || !fs_exists(src)) {
            failed++;
            continue;
        }

        char parent[FS_PATH_MAX];
        path_up(parent, src);
        if (g_xfer.move && !kstrcmp(parent, g_xfer.dest)) {
            skipped++;                   /* 剪切到原目录：无事可做 */
            continue;
        }

        char name[FS_NAME_MAX + 1];
        path_leaf(src, name);
        if (!name[0]) { failed++; continue; }

        char dst[FS_PATH_MAX];
        if (!fm_unique_path(g_xfer.dest, name, dst)) {
            failed++;
            continue;
        }

        int r = fm_copy_tree(src, dst, 0);
        if (r < 0) {
            /* 清掉半成品，避免留下不完整副本。 */
            if (fs_exists(dst)) fm_remove_tree(dst, 0);
            failed++;
            continue;
        }

        if (g_xfer.move) {
            r = fm_remove_tree(src, 0);
            if (r < 0) { failed++; continue; }
        }
        ok++;
    }

    if (g_xfer.move && g_xfer.from_clipboard && (ok + skipped) > 0) {
        g_clip_n = 0;
        g_clip_cut = false;
    }

    char msg[FS_PATH_MAX], b[16];
    if (ok == 0 && failed == 0 && skipped > 0) {
        kstrcpy(msg, "已在此文件夹中");
    } else {
        kstrcpy(msg, g_xfer.move ? "已移动 " : "已复制 ");
        str_app(msg, utoa((uint64_t)ok, b, 10));
        str_app(msg, ok == 1 ? " 项" : " 项");
        if (skipped > 0) {
            str_app(msg, "，已跳过 ");
            str_app(msg, utoa((uint64_t)skipped, b, 10));
        }
        if (failed > 0) {
            str_app(msg, ", ");
            str_app(msg, utoa((uint64_t)failed, b, 10));
            str_app(msg, " 失败");
        }
    }
    fm_status(msg);
    gui_damage();

    /* 保持 busy 直到刷新完成，防止刷新期间的中断启动第二次操作。 */
    files_refresh_all();
    g_fs_busy = 0;
}

/* ---------------- 文件操作 ---------------- */
static void fm_start_prompt(struct fm *fm, uint8_t type) {
    if (!path_on_srfs(fm->path) || fm->viewing) return;
    if (g_fs_busy) {
        fm_status("请等待当前文件操作完成");
        return;
    }
    fm->prompt = true;
    fm->prompt_action = FM_PROMPT_NEW;
    fm->prompt_type = type;
    fm->rename_idx = -1;
    fm->plen = 0;
    fm->pbuf[0] = 0;
    fm->confirm_del = -1;
    fm->props_open = false;
    gui_damage();
}

static void fm_start_rename(struct fm *fm, int idx) {
    if (!path_on_srfs(fm->path) || fm->viewing ||
        idx < 0 || idx >= fm->n) return;
    if (g_fs_busy) {
        fm_status("请等待当前文件操作完成");
        return;
    }
    fm->prompt = true;
    fm->prompt_action = FM_PROMPT_RENAME;
    fm->prompt_type = fm->ents[idx].type;
    fm->rename_idx = idx;
    path_copy(fm->pbuf, fm->ents[idx].name);
    fm->plen = (int)kstrlen(fm->pbuf);
    fm->confirm_del = -1;
    fm->props_open = false;
    gui_damage();
}

static void fm_commit_prompt(struct fm *fm) {
    if (fm->plen > 0) {
        char p[FS_PATH_MAX];
        path_join(p, fm->path, fm->pbuf);

        if (fm->prompt_action == FM_PROMPT_RENAME &&
            fm->rename_idx >= 0 && fm->rename_idx < fm->n) {
            char oldp[FS_PATH_MAX];
            path_join(oldp, fm->path, fm->ents[fm->rename_idx].name);
            if (!kstrcmp(oldp, p)) {
                /* 名字未变，静默完成。 */
            } else {
                int r = fs_rename(oldp, p);
                if (r == FS_ERR_EXIST)
                    fm_status("同名文件已存在");
                else if (r < 0)
                    fm_status("重命名失败");
                else
                    fm_status("已重命名");
            }
        } else {
            if (!fs_exists(p)) {
                int r = fs_create(p, (uint8_t)fm->prompt_type);
                if (r < 0) fm_status("无法创建该项目");
            } else {
                fm_status("同名文件已存在");
            }
        }
    }
    fm->prompt = false;
    fm->prompt_action = FM_PROMPT_NEW;
    fm->rename_idx = -1;
    fm->plen = 0;
    fm->pbuf[0] = 0;
    fm_load(fm);
    gui_damage();
}

static void fm_start_delete(struct fm *fm) {
    if (fm->viewing) return;
    if (g_fs_busy) {
        fm_status("请等待当前文件操作完成");
        return;
    }
    if (!path_on_srfs(fm->path) || !fm_selection_srfs(fm)) {
        fm_status("删除仅支持 StarRing OS 卷");
        gui_damage();
        return;
    }
    int count = fm_sel_count(fm);
    if (count <= 0) return;
    if (fm->sel < 0 || fm->sel >= fm->n) {
        fm->sel = 0;
        while (fm->sel < fm->n && !fm->selected[fm->sel]) fm->sel++;
    }
    if (fm->sel < 0 || fm->sel >= fm->n) return;
    fm->prompt = false;
    fm->confirm_del = fm->sel;
    fm->del_count = count;
    gui_damage();
}

static void fm_commit_delete(struct fm *fm) {
    if (fm->confirm_del < 0) return;
    if (g_fs_busy) {
        fm_status("另一个文件操作仍在进行中");
        return;
    }

    /* 只做路径快照；真正的递归删除投递到主循环执行。 */
    int n = 0;
    for (int i = 0; i < fm->n && i < FS_MAX_DIRENTS && n < FM_XFER_MAX; i++) {
        if (!fm->selected[i]) continue;
        path_join(g_del_items[n].path, fm->path, fm->ents[i].name);
        g_del_items[n].type = fm->ents[i].type;
        n++;
    }

    fm->confirm_del = -1;
    fm->del_count = 0;
    if (n <= 0) { gui_damage(); return; }

    g_del_n = n;
    g_fs_busy = 1;
    if (!gui_post_action(fm_delete_run)) {
        g_del_n = 0;
        g_fs_busy = 0;
        fm_status("文件操作队列已满");
        return;
    }
    fm_status("正在删除...");
}

static void fm_delete_run(void) {
    int ok = 0, failed = 0;
    const char *cwd = fs_getcwd();

    for (int i = 0; i < g_del_n; i++) {
        const char *path = g_del_items[i].path;
        /* 不允许删除 shell 的当前目录或它的祖先。 */
        if (!path_on_srfs(path) || !fs_exists(path) ||
            !cwd || path_within_or_same(cwd, path)) {
            failed++;
            continue;
        }
        int r = fm_tree_fits(path, 0);
        if (r >= 0) r = fm_remove_tree(path, 0);
        if (r < 0) failed++;
        else       ok++;
    }
    g_del_n = 0;

    char msg[FS_PATH_MAX], b[16];
    if (ok == 0 && failed > 0) {
        kstrcpy(msg, "无法删除所选项目");
    } else {
        kstrcpy(msg, "已删除 ");
        str_app(msg, utoa((uint64_t)ok, b, 10));
        str_app(msg, ok == 1 ? " 项" : " 项");
        if (failed > 0) {
            str_app(msg, ", ");
            str_app(msg, utoa((uint64_t)failed, b, 10));
            str_app(msg, " 失败");
        }
    }
    fm_status(msg);

    files_refresh_all();
    g_fs_busy = 0;
}

/* ---------------- 属性对话框 ---------------- */
static void props_rect(struct gui_window *win, int32_t *x, int32_t *y,
                       uint32_t *w, uint32_t *h) {
    uint32_t cw = gui_content_w(win);
    uint32_t bh = fm_body_h(win);
    *w = PROPS_W;
    *h = PROPS_H;
    *x = (cw > PROPS_W) ? (int32_t)((cw - PROPS_W) / 2) : 4;
    *y = body_dy() + ((int32_t)bh - (int32_t)PROPS_H) / 2;
    if (*y < (int32_t)body_dy() + 4) *y = (int32_t)body_dy() + 4;
}

static void props_close_rect(struct gui_window *win, int32_t *x, int32_t *y,
                             uint32_t *w, uint32_t *h) {
    int32_t px, py; uint32_t pw, ph;
    props_rect(win, &px, &py, &pw, &ph);
    *w = PROPS_BTN_W;
    *h = PROPS_BTN_H;
    *x = px + (int32_t)pw - (int32_t)PROPS_BTN_W - 12;
    *y = py + (int32_t)ph - (int32_t)PROPS_BTN_H - 12;
}

static void fm_open_props(struct fm *fm, int idx) {
    if (!fs_ready() || idx < 0 || idx >= fm->n) return;
    char p[FS_PATH_MAX];
    path_join(p, fm->path, fm->ents[idx].name);
    struct fs_dirent st;
    if (fs_stat(p, &st) < 0) return;
    fm->props_idx = idx;
    fm->props_st = st;
    kstrcpy(fm->props_path, p);
    fm->props_open = true;
    fm->confirm_del = -1;
    fm->prompt = false;
    gui_damage();
}

static void fm_close_props(struct fm *fm) {
    if (!fm->props_open) return;
    fm->props_open = false;
    fm->props_idx = -1;
    gui_damage();
}

static void fm_draw_props(struct gui_window *win, struct fm *fm,
                          int32_t cx, int32_t cy) {
    int32_t px, py; uint32_t pw, ph;
    props_rect(win, &px, &py, &pw, &ph);
    px += cx; py += cy;

    gui_fill(px + 6, py + 6, pw, ph, 0x00070D15);          /* 阴影 */
    gui_fill(px, py, pw, ph, GUI_COL_PANEL);
    gui_frame(px, py, pw, ph, GUI_COL_BORDER);
    gui_fill(px + 1, py + 1, pw - 2, 28, GUI_COL_PANEL_LIGHT);
    gui_text(px + 12, py + 7, "属性", GUI_COL_TEXT,
             GUI_COL_PANEL_LIGHT);
    gui_fill(px + 1, py + 29, pw - 2, 1, GUI_COL_BORDER_DARK);

    const char *labels[5] = { "名称", "类型", "大小", "Inode", "修改时间" };
    char values[5][96];
    kstrcpy(values[0], fm->props_st.name);
    kstrcpy(values[1], fm->props_st.type == FS_TYPE_DIR ? "文件夹" : "文件");
    {
        char b[24], sz[24];
        kstrcpy(values[2], utoa(fm->props_st.size, b, 10));
        str_app(values[2], " 字节 (");
        size_str(fm->props_st.size, sz);
        str_app(values[2], sz);
        str_app(values[2], ")");
        kstrcpy(values[3], utoa((uint64_t)fm->props_st.ino, b, 10));
    }
    fmt_mtime(fm->props_st.mtime, values[4]);

    for (int i = 0; i < 5; i++) {
        int32_t ly = py + 40 + i * 24;
        gui_text(px + 16, ly, labels[i], GUI_COL_TEXT_DIM, GUI_COL_PANEL);
        int32_t vx = px + 94;
        int maxc = (int)((pw - 110) / GUI_FONT_W);
        if (maxc < 4) maxc = 4;
        gui_text_max(vx, ly, values[i], maxc, GUI_COL_TEXT, GUI_COL_PANEL);
    }

    int32_t bx, by; uint32_t bw, bh;
    props_close_rect(win, &bx, &by, &bw, &bh);
    bx += cx; by += cy;
    gui_round_fill(bx, by, bw, bh, GUI_COL_BTN, GUI_COL_PANEL);
    gui_frame(bx, by, bw, bh, GUI_COL_ACCENT);
    gui_text(bx + ((int32_t)bw - (int32_t)gui_str_width("关闭")) / 2,
             by + ((int32_t)bh - GUI_FONT_H) / 2, "关闭",
             GUI_COL_WHITE, GUI_COL_BTN);
}

/* ---------------- 绘制原语 ---------------- */
/* 小三角箭头：dir 0=左 1=右 2=上 */
static void draw_tri(int32_t x, int32_t y, int dir, uint32_t col) {
    const int h = 5, wmax = 7;
    for (int i = 0; i < h; i++) {
        int w;
        if (dir == 2) {
            w = 1 + (i * (wmax - 1)) / (h - 1);
            gui_fill(x + (wmax - w) / 2, y + i, (uint32_t)w, 1, col);
        } else {
            w = (i <= h / 2) ? (i + 1) : (h - i);
            w = w * wmax / 3;
            if (w < 1) w = 1;
            if (dir == 1) gui_fill(x, y + i, (uint32_t)w, 1, col);
            else          gui_fill(x + (wmax - w), y + i, (uint32_t)w, 1, col);
        }
    }
}

/* 分组标题前的收起箭头：展开=向下，收起=向右 */
static void draw_chevron(int32_t x, int32_t y, bool open, uint32_t col) {
    for (int i = 0; i < 4; i++) {
        int w = 7 - i * 2;
        if (open) gui_fill(x + i, y + i, (uint32_t)w, 1, col);
        else      gui_fill(x + i, y + i, 1, (uint32_t)w, col);
    }
}

static void draw_btn(int32_t x, int32_t y, uint32_t w, const char *label,
                     bool enabled, bool hot) {
    uint32_t bg = !enabled ? GUI_COL_BTN_DISABLED
                           : (hot ? GUI_COL_BTN_HOVER : GUI_COL_BTN);
    uint32_t bd = !enabled ? 0x00223A52
                           : (hot ? GUI_COL_ACCENT : 0x003A5C80);
    gui_round_fill(x, y, w, CB_H, bg, GUI_COL_PANEL);
    gui_frame(x, y, w, CB_H, bd);
    gui_pixel(x, y, GUI_COL_PANEL);
    gui_pixel(x + (int32_t)w - 1, y, GUI_COL_PANEL);
    uint32_t tw = gui_str_width(label);
    gui_text(x + ((int32_t)w - (int32_t)tw) / 2,
             y + (CB_H - GUI_FONT_H) / 2, label,
             enabled ? GUI_COL_WHITE : 0x00506A82, bg);
}

static void draw_abtn(int32_t x, int32_t y, int dir, bool enabled, bool hot) {
    uint32_t bg = !enabled ? GUI_COL_BTN_DISABLED
                           : (hot ? GUI_COL_BTN_HOVER : GUI_COL_PANEL_LIGHT);
    gui_round_fill(x, y, ABTN_SZ, ABTN_SZ, bg, GUI_COL_WINDOW_BG);
    gui_frame(x, y, ABTN_SZ, ABTN_SZ, enabled ? 0x003A5C80 : 0x00223A52);
    gui_pixel(x, y, GUI_COL_WINDOW_BG);
    gui_pixel(x + ABTN_SZ - 1, y, GUI_COL_WINDOW_BG);
    draw_tri(x + (ABTN_SZ - 7) / 2, y + (ABTN_SZ - 5) / 2, dir,
             enabled ? GUI_COL_TEXT : 0x00506A82);
}

/* 文件夹图标右下角的小徽标（18x18） */
static void draw_badge(int32_t x, int32_t y, int kind) {
    gui_fill(x, y, 18, 18, 0x00F2F6FA);
    gui_fill(x, y, 18, 1, GUI_COL_WHITE);
    gui_fill(x, y + 17, 18, 1, 0x00B9C4D0);
    gui_fill(x + 17, y, 1, 18, 0x00D8E0E8);

    switch (kind) {
    case BD_IMAGE:
        gui_fill(x + 2, y + 9, 14, 7, 0x004E9E5B);      /* 草地 */
        gui_fill(x + 10, y + 3, 5, 5, 0x00F2C14E);      /* 太阳 */
        for (int i = 0; i < 7; i++)                     /* 山 */
            gui_fill(x + 3 + i, y + 14 - i, 1, (uint32_t)(i + 1), 0x002F6FBF);
        break;
    case BD_MUSIC:
        gui_fill(x + 5, y + 4, 2, 9, 0x003E6FB0);       /* 符干 */
        gui_fill(x + 11, y + 3, 2, 10, 0x003E6FB0);
        gui_fill(x + 5, y + 3, 8, 2, 0x003E6FB0);       /* 符头横梁 */
        gui_fill(x + 2, y + 11, 5, 4, 0x0059B0F5);      /* 音符 */
        gui_fill(x + 8, y + 10, 5, 4, 0x0059B0F5);
        break;
    case BD_VIDEO:
        gui_fill(x + 2, y + 3, 14, 12, 0x006A4FA8);     /* 胶片 */
        gui_fill(x + 4, y + 5, 10, 8, 0x00E8EEF5);
        draw_tri(x + 7, y + 6, 1, 0x006A4FA8);
        break;
    case BD_DOC:
        gui_fill(x + 4, y + 2, 10, 14, 0x00A8B4C4);
        gui_fill(x + 5, y + 3, 8, 12, GUI_COL_WHITE);
        for (int i = 0; i < 4; i++)
            gui_fill(x + 6, y + 5 + i * 3, (uint32_t)(i == 3 ? 4 : 6), 1, 0x007C93AC);
        break;
    case BD_DOWNLOAD:
        gui_fill(x + 7, y + 2, 4, 8, 0x003E86D8);       /* 竖杆 */
        gui_fill(x + 3, y + 8, 12, 3, 0x003E86D8);      /* 箭头横 */
        gui_fill(x + 5, y + 11, 8, 2, 0x003E86D8);
        gui_fill(x + 7, y + 13, 4, 2, 0x003E86D8);
        break;
    case BD_DESKTOP:
        gui_fill(x + 2, y + 3, 14, 10, 0x002F6FBF);     /* 屏幕 */
        gui_fill(x + 3, y + 4, 12, 8, 0x007CC4F5);
        gui_fill(x + 7, y + 13, 4, 2, 0x002F6FBF);
        break;
    default:
        break;
    }
}

static void draw_icon_folder_big(int32_t x, int32_t y, int badge) {
    const uint32_t edge = 0x00C8942F, body = 0x00F5C542, hi = 0x00FFE48A;
    gui_fill(x + 2, y, 20, 9, edge);                /* 标签页 */
    gui_fill(x + 3, y + 1, 18, 6, 0x00E6B23A);
    gui_fill(x, y + 7, 46, 35, edge);               /* 主体 */
    gui_fill(x + 1, y + 8, 44, 33, body);
    gui_fill(x + 1, y + 8, 44, 2, hi);
    if (badge) draw_badge(x + 27, y + 22, badge);
}

static void draw_icon_file_big(int32_t x, int32_t y) {
    gui_fill(x + 5, y, 34, 44, 0x00A8B4C4);
    gui_fill(x + 6, y + 1, 32, 42, GUI_COL_WHITE);
    gui_fill(x + 29, y + 1, 9, 9, 0x00D6DEE8);      /* 折角 */
    gui_fill(x + 29, y + 9, 9, 1, 0x00A8B4C4);
    for (int i = 0; i < 5; i++)
        gui_fill(x + 10, y + 15 + i * 5, (uint32_t)(i == 4 ? 16 : 24), 2,
                 0x00A8C4E0);
}

/* ---------------- 绘制：各区域 ---------------- */
static void draw_section_title(int32_t x, int32_t y, uint32_t w,
                               const char *title, int count, bool open,
                               bool hot) {
    uint32_t col = hot ? GUI_COL_TEXT : GUI_COL_TEXT_DIM;
    gui_fill(x, y, w, SEC_H, GUI_COL_WINDOW_BG);
    draw_chevron(x + 6, y + (SEC_H - 7) / 2, open, col);

    int32_t tx = x + 18;
    int32_t ty = y + (SEC_H - GUI_FONT_H) / 2;
    gui_text(tx, ty, title, col, GUI_COL_WINDOW_BG);
    tx += (int32_t)gui_str_width(title);

    char cnt[24], nb[24];
    kstrcpy(cnt, " (");
    str_app(cnt, utoa((uint64_t)count, nb, 10));
    str_app(cnt, ")");
    gui_text(tx, ty, cnt, col, GUI_COL_WINDOW_BG);
    tx += (int32_t)gui_str_width(cnt) + 10;

    if (tx < x + (int32_t)w)
        gui_fill(tx, y + SEC_H / 2, (uint32_t)(x + (int32_t)w - tx), 1,
                 GUI_COL_BORDER_DARK);
}

static void draw_tile(struct fm *fm, int idx, int32_t x, int32_t y,
                      bool sel, bool hot) {
    struct fs_dirent *e = &fm->ents[idx];
    bool drop = false;
    if (e->type == FS_TYPE_DIR && fm->file_drag && fm->drop_valid) {
        char p[FS_PATH_MAX];
        path_join(p, fm->path, e->name);
        drop = !kstrcmp(p, fm->drop_path);
    }
    bool cut = !sel && fm_item_is_cut(fm, idx);

    uint32_t bg = sel ? GUI_COL_SELECT
                      : (drop ? 0x00224C73
                              : (hot ? GUI_COL_SELECT_HOVER : GUI_COL_WINDOW_BG));
    gui_round_fill(x, y, TILE_W, TILE_H, bg, GUI_COL_WINDOW_BG);
    if (sel) gui_frame(x, y, TILE_W, TILE_H, GUI_COL_ACCENT);
    if (drop) gui_frame(x + 1, y + 1, TILE_W - 2, TILE_H - 2, GUI_COL_ACCENT);
    if (cut) gui_frame(x + 1, y + 1, TILE_W - 2, TILE_H - 2, 0x00C8942F);

    if (e->type == FS_TYPE_DIR)
        draw_icon_folder_big(x + 8, y + 8, folder_badge(e->name));
    else
        draw_icon_file_big(x + 8, y + 7);

    int maxc = (int)((TILE_W - 66) / GUI_FONT_W);
    if (maxc < 4) maxc = 4;
    int32_t ty = y + (int32_t)TILE_H / 2 - GUI_FONT_H / 2;
    if (e->type == FS_TYPE_FILE) ty -= 8;          /* 文件：名字上移一行 */
    uint32_t name_fg = sel ? GUI_COL_WHITE
                           : (cut ? 0x00C8942F : GUI_COL_TEXT);
    gui_text_max(x + 62, ty, e->name, maxc, name_fg, bg);
    if (e->type == FS_TYPE_FILE) {
        char sz[24];
        size_str(e->size, sz);
        gui_text_max(x + 62, ty + GUI_FONT_H, sz, maxc,
                     sel ? GUI_COL_WHITE : GUI_COL_TEXT_DIM, bg);
    }
}

static void draw_sidebar(struct fm *fm, int32_t x, int32_t y, uint32_t w,
                         uint32_t h) {
    gui_fill(x, y, w, h, GUI_COL_PANEL);
    gui_fill(x + (int32_t)w - 1, y, 1, h, GUI_COL_BORDER_DARK);

    static const char *const hdr[2] = { "快速访问", "此电脑" };
    int32_t iy = y + 4;
    for (int g = 0; g < 2; g++) {
        gui_text(x + 12, iy + (SIDE_HDR_H - GUI_FONT_H) / 2, hdr[g],
                 GUI_COL_TEXT_DIM, GUI_COL_PANEL);
        iy += SIDE_HDR_H;

        for (int i = 0; i < fm->side_n; i++) {
            if (fm->side[i].group != (uint8_t)g) continue;
            const struct side_item *s = &fm->side[i];
            bool cur = !fm->viewing && !kstrcmp(fm->path, s->path);
            bool hot = (fm->hside == i);
            bool drop = fm_drop_is(fm, s->path);
            uint32_t bg = cur ? GUI_COL_SELECT
                              : (drop ? 0x00224C73
                                      : (hot ? GUI_COL_SELECT_HOVER
                                             : GUI_COL_PANEL));
            gui_round_fill(x + 6, iy, w - 12, SIDE_ROW_H, bg, GUI_COL_PANEL);
            if (cur) gui_fill(x + 6, iy + 3, 3, SIDE_ROW_H - 6, GUI_COL_ACCENT);
            if (drop) gui_frame(x + 6, iy, w - 12, SIDE_ROW_H, GUI_COL_ACCENT);

            if (s->kind == SIDE_DRIVE)
                gui_draw_mini_icon(x + 14, iy + 3, GUI_ICON_DISK);
            else if (s->kind == SIDE_USB)
                gui_draw_mini_icon(x + 14, iy + 3, GUI_ICON_USB);
            else
                gui_icon_folder(x + 14, iy + 4);

            int maxc = (int)((w - 48) / GUI_FONT_W);
            if (maxc < 4) maxc = 4;
            gui_text_max(x + 36, iy + (SIDE_ROW_H - GUI_FONT_H) / 2, s->label,
                         maxc, cur ? GUI_COL_WHITE : GUI_COL_TEXT, bg);
            iy += SIDE_ROW_H;
        }
        iy += SIDE_GAP;
    }
}

static bool drive_usage(const char *path, uint64_t *tot_blk, uint64_t *free_blk) {
    uint64_t ti = 0, fi = 0;
    *tot_blk = *free_blk = 0;
    if (fs_statfs_path(path, tot_blk, free_blk, &ti, &fi) != FS_OK ||
        *tot_blk == 0) {
        *tot_blk = *free_blk = 0;
        return false;
    }
    return true;
}

static void draw_drive_row(struct fm *fm, int idx, int32_t x, int32_t y,
                           uint32_t w) {
    if (idx < 0 || idx >= fm->mount_n) return;
    const struct vfs_mount_info *m = &fm->mounts[idx];
    bool drop = fm_drop_is(fm, m->path);
    bool hot  = (fm->hdrive == idx);
    if (hot || drop)
        gui_round_fill(x + 4, y + 2, w - 8, DRIVE_H - 4,
                       drop ? 0x00224C73 : GUI_COL_SELECT_HOVER,
                       GUI_COL_WINDOW_BG);
    if (drop)
        gui_frame(x + 4, y + 2, w - 8, DRIVE_H - 4, GUI_COL_ACCENT);

    gui_icon_disk(x + 14, y + 10);                 /* 46x36 */
    int32_t tx = x + 74;
    char label[48];
    mount_label(m, label, sizeof(label));
    gui_text_bold(tx, y + 6, label, GUI_COL_TEXT, GUI_COL_WINDOW_BG);

    uint32_t bar_w = 260;
    if (bar_w + 90 > w) bar_w = w > 90 ? w - 90 : 20;
    int32_t by = y + 24;

    uint64_t tb = 0, fb = 0;
    if (drive_usage(m->path, &tb, &fb)) {
        uint64_t used = tb - fb;
        gui_fill(tx, by, bar_w, 12, 0x00C8CED6);       /* 轨道 */
        uint32_t fw = (uint32_t)((uint64_t)bar_w * used / tb);
        if (fw > bar_w) fw = bar_w;
        gui_fill(tx, by, fw, 12, GUI_COL_ACCENT);      /* 已用 */

        char buf[80], b1[24], b2[24];
        kstrcpy(buf, utoa(fb * 4096 / 1024 / 1024, b1, 10));
        str_app(buf, " MB 可用 / 共 ");
        str_app(buf, utoa(tb * 4096 / 1024 / 1024, b2, 10));
        str_app(buf, " MB");
        gui_text(tx, by + 14, buf, GUI_COL_TEXT_DIM, GUI_COL_WINDOW_BG);
    } else {
        gui_fill(tx, by, bar_w, 12, GUI_COL_BTN_DISABLED);
        gui_text(tx, by + 14, "未挂载", GUI_COL_TEXT_DIM,
                 GUI_COL_WINDOW_BG);
    }
}

/* 地址栏：< > ^  +  面包屑 */
static void draw_addr_bar(struct gui_window *win, struct fm *fm) {
    int32_t  cx = gui_content_x(win);
    int32_t  cy = gui_content_y(win) + CMD_H;
    uint32_t cw = gui_content_w(win);

    gui_fill(cx, cy, cw, ADDR_H, GUI_COL_WINDOW_BG);
    gui_fill(cx, cy, cw, 1, GUI_COL_BORDER_DARK);
    gui_fill(cx, cy + ADDR_H - 1, cw, 1, GUI_COL_BORDER_DARK);

    bool viewing  = fm->viewing;
    bool can_back = !viewing && fm->back_n > 0;
    bool can_fwd  = !viewing && fm->fwd_n > 0;
    bool can_up   = !viewing && !path_is_root(fm->path);

    int32_t ay = gui_content_y(win) + abtn_dy();
    draw_abtn(cx + abtn_dx(AB_BACK), ay, 0, can_back, fm->hab == AB_BACK);
    draw_abtn(cx + abtn_dx(AB_FWD),  ay, 1, can_fwd,  fm->hab == AB_FWD);
    draw_abtn(cx + abtn_dx(AB_UP),   ay, 2, can_up,   fm->hab == AB_UP);

    int32_t tx = cx + abtn_dx(AB_N - 1) + ABTN_SZ + 10;
    int32_t ty = cy + (ADDR_H - GUI_FONT_H) / 2;
    int32_t maxx = cx + (int32_t)cw - 12;

    if (viewing) {
        gui_icon_file(tx, ty + 1);
        tx += 20;
        gui_text_max(tx, ty, fm->file, (maxx - tx) / GUI_FONT_W, GUI_COL_TEXT,
                     GUI_COL_WINDOW_BG);
        return;
    }

    gui_draw_mini_icon(tx, ty, GUI_ICON_PC);
    tx += 22;

    /* 面包屑：This PC > home > guest */
    const char *seg = fm->path;
    while (*seg == '/') seg++;
    gui_text(tx, ty, "此电脑", GUI_COL_TEXT_DIM, GUI_COL_WINDOW_BG);
    tx += (int32_t)gui_str_width("此电脑");

    while (*seg) {
        const char *end = seg;
        while (*end && *end != '/') end++;
        int len = (int)(end - seg);
        char part[FS_NAME_MAX + 1];
        if (len > FS_NAME_MAX) len = FS_NAME_MAX;
        for (int i = 0; i < len; i++) part[i] = seg[i];
        part[len] = 0;

        int32_t need = (int32_t)gui_str_width("  >  ") +
                       (int32_t)gui_str_width(part);
        if (tx + need > maxx) break;

        gui_text(tx, ty, "  >  ", GUI_COL_TEXT_DIM, GUI_COL_WINDOW_BG);
        tx += (int32_t)gui_str_width("  >  ");
        gui_text(tx, ty, part, GUI_COL_TEXT, GUI_COL_WINDOW_BG);
        tx += (int32_t)gui_str_width(part);

        seg = end;
        while (*seg == '/') seg++;
    }
}

static void draw_status_bar(struct gui_window *win, struct fm *fm) {
    int32_t cx = gui_content_x(win);
    uint32_t cw = gui_content_w(win);
    int32_t sy = gui_content_y(win) + (int32_t)gui_content_h(win) - STATUS_H;

    gui_fill(cx, sy, cw, STATUS_H, GUI_COL_PANEL);
    gui_fill(cx, sy, cw, 1, GUI_COL_BORDER_DARK);
    int32_t ty = sy + (STATUS_H - GUI_FONT_H) / 2;

    if (fm->prompt) {
        char buf[FS_NAME_MAX + 40];
        if (fm->prompt_action == FM_PROMPT_RENAME)
            kstrcpy(buf, "重命名为: ");
        else
            kstrcpy(buf, fm->prompt_type == FS_TYPE_FILE ? "新文件名: "
                                                         : "新文件夹名: ");
        str_app(buf, fm->pbuf);
        str_app(buf, "_");
        gui_text(cx + 8, ty, buf, GUI_COL_ACCENT, GUI_COL_PANEL);
        const char *hint = (fm->prompt_action == FM_PROMPT_RENAME)
                               ? "回车 = 重命名    Esc = 取消"
                               : "回车 = 创建    Esc = 取消";
        gui_text(cx + (int32_t)cw - (int32_t)gui_str_width(hint) - 8, ty, hint,
                 GUI_COL_TEXT_DIM, GUI_COL_PANEL);
        return;
    }

    if (fm->confirm_del >= 0) {
        char buf[160], b1[24];
        if (fm->del_count > 1) {
            kstrcpy(buf, "删除 ");
            str_app(buf, utoa((uint64_t)fm->del_count, b1, 10));
            str_app(buf, " 个选中项?");
        } else {
            kstrcpy(buf, "删除“");
            bool dir = false;
            if (fm->confirm_del < fm->n) {
                str_app(buf, fm->ents[fm->confirm_del].name);
                dir = (fm->ents[fm->confirm_del].type == FS_TYPE_DIR);
            }
            str_app(buf, dir ? "”及其内容?" : "”?");
        }
        gui_text(cx + 8, ty, buf, 0x00FF6B5E, GUI_COL_PANEL);
        const char *hint = "Y = 删除    N = 取消";
        gui_text(cx + (int32_t)cw - (int32_t)gui_str_width(hint) - 8, ty, hint,
                 GUI_COL_TEXT_DIM, GUI_COL_PANEL);
        return;
    }

    if (g_status[0]) {
        if (timer_ticks() - g_status_tick < FM_STATUS_TICKS) {
            gui_text(cx + 8, ty, g_status, GUI_COL_ACCENT, GUI_COL_PANEL);
            return;
        }
        g_status[0] = 0;
    }

    char buf[192], b1[24];
    if (fm->viewing) {
        kstrcpy(buf, path_base(fm->file));
        str_app(buf, "    ");
        str_app(buf, utoa(fm->fsize, b1, 10));
        str_app(buf, " 字节");
        if (fm->fsize > (uint64_t)fm->vlen) {
            char b2[24];
            str_app(buf, "  (仅显示前 ");
            str_app(buf, utoa((uint64_t)fm->vlen, b2, 10));
            str_app(buf, " 字节)");
        }
        gui_text(cx + 8, ty, buf, GUI_COL_TEXT_DIM, GUI_COL_PANEL);
        const char *hint = "上下/翻页滚动    退格 / Esc = 返回";
        gui_text(cx + (int32_t)cw - (int32_t)gui_str_width(hint) - 8, ty, hint,
                 GUI_COL_TEXT_DIM, GUI_COL_PANEL);
        return;
    }

    kstrcpy(buf, utoa((uint64_t)fm->n, b1, 10));
    str_app(buf, (fm->n == 1) ? " 项" : " 项");
    int sc = fm_sel_count(fm);
    if (sc > 0) {
        char b2[24];
        str_app(buf, "    已选: ");
        str_app(buf, utoa((uint64_t)sc, b2, 10));
        str_app(buf, (sc == 1) ? " 项" : " 项");
    }
    gui_text(cx + 8, ty, buf, GUI_COL_TEXT_DIM, GUI_COL_PANEL);

    if (fs_ready()) {
        uint64_t tb = 0, fb = 0;
        if (drive_usage(fm->path, &tb, &fb)) {
            char r[64], b2[24], b3[24];
            kstrcpy(r, utoa(fb * 4096 / 1024 / 1024, b2, 10));
            str_app(r, " MB 可用 / 共 ");
            str_app(r, utoa(tb * 4096 / 1024 / 1024, b3, 10));
            str_app(r, " MB");
            gui_text(cx + (int32_t)cw - (int32_t)gui_str_width(r) - 8, ty, r,
                     GUI_COL_TEXT_DIM, GUI_COL_PANEL);
        }
    }
}

/* ---------------- 绘制：文本查看器 ---------------- */
static void fm_draw_viewer(struct fm *fm, int32_t cx, int32_t cy,
                           uint32_t cw, uint32_t ch) {
    gui_fill(cx, cy, cw, ch, GUI_COL_WINDOW_BG);

    int maxc = (int)((cw - 28) / GUI_FONT_W);
    int rows = (int)(ch / GUI_FONT_H);
    if (maxc < 1) maxc = 1;
    if (rows < 1) rows = 1;

    int total = viewer_lines(fm);
    uint64_t maxscroll = (total > rows) ? (uint64_t)(total - rows) : 0;
    if (fm->vscroll > maxscroll) fm->vscroll = maxscroll;

    uint64_t line_no = 0;
    int drawn = 0;
    int i = 0;
    while (i <= fm->vlen && drawn < rows) {
        int start = i;
        while (i < fm->vlen && fm->vbuf[i] != '\n') i++;
        int end = i;
        if (i < fm->vlen) i++;
        if (line_no++ < fm->vscroll) continue;

        char tmp[256];
        int len = end - start;
        if (len > maxc) len = maxc;
        if (len > (int)sizeof(tmp) - 1) len = (int)sizeof(tmp) - 1;
        for (int k = 0; k < len; k++) {
            char c = (char)fm->vbuf[start + k];
            tmp[k] = (c >= 32 && c < 127) ? c : ' ';
        }
        tmp[len] = 0;
        gui_text(cx + 8, cy + 4 + drawn * GUI_FONT_H, tmp,
                 GUI_COL_TEXT, GUI_COL_WINDOW_BG);
        drawn++;
        if (i >= fm->vlen) break;
    }

    if (fm->vlen == 0)
        gui_text(cx + 8, cy + 4, "(空文件)", GUI_COL_TEXT_DIM,
                 GUI_COL_WINDOW_BG);

    /* 右侧滚动条 */
    int32_t track_x = (int32_t)(cx + cw - 6);
    int32_t track_y = cy + 2;
    int32_t track_h = (int32_t)ch - 4;
    if (track_h > 8) {
        gui_fill(track_x, track_y, 4, (uint32_t)track_h, GUI_COL_PANEL);
        if (total > rows) {
            uint32_t th = (uint32_t)((uint64_t)track_h * (uint64_t)rows /
                                     (uint64_t)total);
            if (th < 12) th = 12;
            if (th > (uint32_t)track_h) th = (uint32_t)track_h;
            uint32_t range = (uint32_t)track_h - th;
            int32_t ty = track_y +
                (int32_t)(maxscroll ? (fm->vscroll * (uint64_t)range / maxscroll)
                                    : 0);
            gui_fill(track_x, ty, 4, th, GUI_COL_ACCENT);
        } else {
            gui_fill(track_x, track_y, 4, (uint32_t)track_h,
                     GUI_COL_PANEL_LIGHT);
        }
    }
}

/* 半透明观感的框选框：隔行填色 + 虚线边框（无 alpha 时的廉价近似）。 */
static void draw_rubber_band(struct fm *fm, int32_t ox, int32_t oy) {
    if (!fm->drag_active) return;
    int32_t x0 = fm->drag_x0, y0 = fm->drag_y0;
    int32_t x1 = fm->drag_x1, y1 = fm->drag_y1;
    if (x1 < x0) { int32_t t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int32_t t = y0; y0 = y1; y1 = t; }
    if (x1 - x0 < 1 || y1 - y0 < 1) return;
    x0 += ox; x1 += ox; y0 += oy; y1 += oy;

    /* 50% 隔行淡蓝（近似透明），再叠 2px 虚线边框。 */
    for (int32_t y = y0 + 1; y < y1; y += 2)
        gui_fill(x0 + 1, y, (uint32_t)(x1 - x0 - 1), 1, 0x001E3550);

    for (int32_t x = x0; x < x1; x++) {
        if (((x - x0) / 4) & 1) continue;
        gui_fill(x, y0, 1, 2, GUI_COL_ACCENT);
        gui_fill(x, y1 - 1, 1, 2, GUI_COL_ACCENT);
    }
    for (int32_t y = y0; y < y1; y++) {
        if (((y - y0) / 4) & 1) continue;
        gui_fill(x0, y, 2, 1, GUI_COL_ACCENT);
        gui_fill(x1 - 1, y, 2, 1, GUI_COL_ACCENT);
    }
}

/* 拖动文件时跟随指针的小标签。 */
static void fm_drag_label(struct fm *fm, char *out) {
    int n = fm_sel_count(fm);
    if (n == 1 && fm->sel >= 0 && fm->sel < fm->n) {
        path_copy(out, fm->ents[fm->sel].name);
        return;
    }
    char b[16];
    kstrcpy(out, utoa((uint64_t)n, b, 10));
    str_app(out, n == 1 ? " 项" : " 项");
}

static void fm_ghost_rect(struct fm *fm, int32_t gx, int32_t gy,
                          int32_t *out_x, int32_t *out_y,
                          uint32_t *out_w, uint32_t *out_h) {
    char label[FS_NAME_MAX + 24];
    fm_drag_label(fm, label);
    uint32_t w = gui_str_width(label) + 58;
    if (w < 170) w = 170;
    if (w > 300) w = 300;
    uint32_t h = 30;

    int32_t cw = (int32_t)gui_content_w(fm->win);
    int32_t ch = (int32_t)gui_content_h(fm->win);
    int32_t x = gx + 14, y = gy + 14;
    if (x + (int32_t)w > cw - 4) x = cw - 4 - (int32_t)w;
    if (y + (int32_t)h > ch - 4) y = ch - 4 - (int32_t)h;
    if (x < 4) x = 4;
    if (y < 4) y = 4;
    *out_x = x; *out_y = y; *out_w = w; *out_h = h;
}

static void fm_draw_drag_ghost(struct fm *fm, int32_t ox, int32_t oy) {
    if (!fm->file_drag || fm_sel_count(fm) <= 0) return;

    int32_t x, y; uint32_t w, h;
    fm_ghost_rect(fm, fm->drag_cx, fm->drag_cy, &x, &y, &w, &h);
    x += ox; y += oy;

    gui_fill(x + 4, y + 4, w, h, 0x00060C14);
    gui_fill(x, y, w, h, GUI_COL_PANEL_LIGHT);
    gui_frame(x, y, w, h, GUI_COL_ACCENT);

    int idx = fm->sel;
    if (idx >= 0 && idx < fm->n && fm->ents[idx].type == FS_TYPE_DIR)
        gui_icon_folder(x + 7, y + 6);
    else
        gui_icon_file(x + 7, y + 6);

    char label[FS_NAME_MAX + 24];
    fm_drag_label(fm, label);
    gui_text_max(x + 34, y + (int32_t)(h - GUI_FONT_H) / 2, label,
                 (int)((w - 42) / GUI_FONT_W), GUI_COL_WHITE,
                 GUI_COL_PANEL_LIGHT);
}

/* ---------------- 绘制：主入口 ---------------- */
static void fm_draw(struct gui_window *win) {
    struct fm *fm = win->data;
    int32_t  cx = gui_content_x(win);
    int32_t  cy = gui_content_y(win);
    uint32_t cw = gui_content_w(win);

    /* 命令栏 */
    gui_fill(cx, cy, cw, CMD_H, GUI_COL_PANEL);
    gui_fill(cx, cy + CMD_H - 1, cw, 1, GUI_COL_BORDER_DARK);
    {
        bool en[CB_N];
        en[CB_NEW]     = !fm->viewing && path_on_srfs(fm->path);
        en[CB_DEL]     = !fm->viewing && fm_selection_srfs(fm);
        en[CB_REFRESH] = !fm->viewing;
        for (int i = 0; i < CB_N; i++)
            draw_btn(cx + cbtn_dx(i), cy + CB_DY, cb_w[i], cb_label[i],
                     en[i], fm->hcmd == i);
    }

    draw_addr_bar(win, fm);

    if (fm->viewing) {
        fm_draw_viewer(fm, cx, (int32_t)(cy + body_dy()), cw, fm_body_h(win));
        draw_status_bar(win, fm);
        return;
    }

    struct layout L;
    layout_calc(win, fm, &L);

    /* 内容区底色 + 导航窗格 */
    gui_fill(cx, cy + body_dy(), cw, fm_body_h(win), GUI_COL_WINDOW_BG);
    draw_sidebar(fm, cx, cy + body_dy(), NAV_W, fm_body_h(win));

    /* Folders 分组 */
    bool root = path_is_root(fm->path);
    draw_section_title(cx + L.mx, cy + L.sec_y, L.mw,
                       root ? "文件夹" : "项目", fm->n, fm->folders_open,
                       fm->hsec == HS_FOLDERS);

    if (fm->folders_open && L.grid_h > 0) {
        fm_ensure_visible(fm, &L);
        if (fm->n == 0) {
            if (!fs_ready()) {
                const char *msg;
                if (!partition_valid())
                    msg = "未找到文件系统（没有 MBR 分区）";
                else if (partition_count() == 0)
                    msg = "找到 MBR，但没有可用分区";
                else
                    msg = "找到 MBR，但无法挂载 SRFS 分区";
                gui_text(cx + L.mx + 8, cy + L.grid_y + 10, msg,
                         GUI_COL_CLOSE, GUI_COL_WINDOW_BG);
                gui_text(cx + L.mx + 8, cy + L.grid_y + 30,
                         partition_count() == 0
                             ? "打开磁盘工具初始化此磁盘"
                             : "在终端运行 'lsblk' 查看详情",
                         GUI_COL_TEXT_DIM, GUI_COL_WINDOW_BG);
            } else {
                gui_text(cx + L.mx + 8, cy + L.grid_y + 10,
                         "此文件夹为空",
                         GUI_COL_TEXT_DIM, GUI_COL_WINDOW_BG);
            }
        }
        for (int i = 0; i < L.visible; i++) {
            int idx = fm->top + i;
            if (idx >= fm->n) break;
            int32_t tx, ty;
            if (!tile_dx(fm, &L, idx, &tx, &ty)) continue;
            draw_tile(fm, idx, cx + tx, cy + ty,
                      idx < FS_MAX_DIRENTS && fm->selected[idx],
                      idx == fm->htile);
        }
        draw_rubber_band(fm, cx, cy);
    }

    /* Devices and drives 分组 */
    if (L.drv_y && fm->mount_n > 0) {
        draw_section_title(cx + L.mx, cy + L.drv_y, L.mw, "设备和驱动器",
                           fm->mount_n, fm->drives_open, fm->hsec == HS_DRIVES);
        if (fm->drives_open) {
            int dn = (int)(L.drv_h / DRIVE_H);
            for (int i = 0; i < dn && i < fm->mount_n; i++)
                draw_drive_row(fm, i, cx + L.mx,
                               cy + L.drv_y + SEC_H + i * DRIVE_H, L.mw);
        }
    }

    draw_status_bar(win, fm);
    fm_draw_drag_ghost(fm, cx, cy);
    if (fm->props_open) fm_draw_props(win, fm, cx, cy);
}

/* ---------------- 局部重绘 ---------------- */
static void dmg_rel(struct gui_window *win, int32_t x, int32_t y,
                    uint32_t w, uint32_t h) {
    if ((int32_t)w <= 0 || (int32_t)h <= 0) return;
    gui_damage_rect(gui_content_x(win) + x, gui_content_y(win) + y, w, h);
}

static void dmg_cmd(struct gui_window *win, int i) {
    if (i < 0 || i >= CB_N) return;
    dmg_rel(win, cbtn_dx(i) - 1, CB_DY - 1, cb_w[i] + 2, CB_H + 2);
}

static void dmg_ab(struct gui_window *win, int i) {
    if (i < 0 || i >= AB_N) return;
    dmg_rel(win, abtn_dx(i), abtn_dy(), ABTN_SZ, ABTN_SZ);
}

static void dmg_side(struct gui_window *win, struct fm *fm, int i) {
    if (i < 0 || i >= fm->side_n) return;
    int32_t y = side_dy(fm, i);
    if (y < 0) return;
    dmg_rel(win, 4, y - 1, NAV_W - 8, SIDE_ROW_H + 2);
}

static void dmg_tile(struct gui_window *win, struct fm *fm, int idx) {
    if (idx < 0 || idx >= fm->n) return;
    struct layout L;
    layout_calc(win, fm, &L);
    int32_t x, y;
    if (!tile_dx(fm, &L, idx, &x, &y)) return;
    dmg_rel(win, x, y, TILE_W, TILE_H);
}

/* 选择数量会改变状态栏和命令栏 Delete 的可用状态。 */
static void dmg_chrome(struct gui_window *win) {
    uint32_t cw = gui_content_w(win);
    uint32_t ch = gui_content_h(win);
    dmg_rel(win, 0, 0, cw, CMD_H);
    dmg_rel(win, 0, (int32_t)ch - STATUS_H, cw, STATUS_H);
}

static void dmg_grid(struct gui_window *win, struct fm *fm) {
    struct layout L;
    layout_calc(win, fm, &L);
    if (L.grid_h == 0) return;
    dmg_rel(win, L.mx, L.grid_y, L.mw, L.grid_h);
    dmg_chrome(win);
}

static int grid_hit_idx(struct fm *fm, struct layout *L,
                        int32_t cx, int32_t cy) {
    if (L->grid_h == 0 || cx < L->mx || cy < L->grid_y ||
        cy >= L->grid_y + (int32_t)L->grid_h) return -1;
    int col = (cx - L->mx) / (int32_t)TILE_W;
    int row = (cy - L->grid_y) / (int32_t)TILE_H;
    if (col < 0 || col >= L->cols || row < 0 || row >= L->rows) return -1;
    int idx = fm->top + row * L->cols + col;
    return (idx >= 0 && idx < fm->n) ? idx : -1;
}

static void dmg_box(struct gui_window *win, int32_t x0, int32_t y0,
                    int32_t x1, int32_t y1) {
    if (x0 > x1) { int32_t t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int32_t t = y0; y0 = y1; y1 = t; }
    if (x1 - x0 < 1) x1 = x0 + 1;
    if (y1 - y0 < 1) y1 = y0 + 1;
    dmg_rel(win, x0 - 1, y0 - 1, (uint32_t)(x1 - x0 + 3),
            (uint32_t)(y1 - y0 + 3));
}

/* ---------------- 文件拖放 ---------------- */
static bool fm_drop_allowed(struct fm *fm, const char *dest) {
    if (!dest || !dest[0] || !path_on_srfs(dest)) return false;
    if (!kstrcmp(dest, fm->path)) return false;      /* 拖回当前目录无意义 */
    if (fm_sel_count(fm) <= 0) return false;

    for (int i = 0; i < fm->n && i < FS_MAX_DIRENTS; i++) {
        if (!fm->selected[i]) continue;
        char src[FS_PATH_MAX];
        path_join(src, fm->path, fm->ents[i].name);
        if (!path_on_srfs(src)) return false;
        if (path_within_or_same(dest, src)) return false;
    }
    return true;
}

static bool fm_drop_target(struct gui_window *win, struct fm *fm,
                           int32_t cx, int32_t cy, char *out) {
    char cand[FS_PATH_MAX];
    cand[0] = 0;
    int32_t by = body_dy();

    if (cx < NAV_W && cy >= by && cy < by + (int32_t)fm_body_h(win)) {
        for (int i = 0; i < fm->side_n; i++) {
            int32_t y = side_dy(fm, i);
            if (y >= 0 && cy >= y && cy < y + SIDE_ROW_H) {
                path_copy(cand, fm->side[i].path);
                break;
            }
        }
    } else {
        struct layout L;
        layout_calc(win, fm, &L);
        int idx = grid_hit_idx(fm, &L, cx, cy);
        if (idx >= 0 && fm->ents[idx].type == FS_TYPE_DIR) {
            path_join(cand, fm->path, fm->ents[idx].name);
        } else if (L.drv_y && cy >= L.drv_y + SEC_H &&
                   cy < L.drv_y + SEC_H + (int32_t)L.drv_h) {
            int di = (cy - (L.drv_y + SEC_H)) / DRIVE_H;
            if (di >= 0 && di < fm->mount_n)
                path_copy(cand, fm->mounts[di].path);
        }
    }

    if (!cand[0] || !fm_drop_allowed(fm, cand)) return false;
    path_copy(out, cand);
    return true;
}

static void fm_file_drag_begin(struct gui_window *win, struct fm *fm,
                               int32_t cx, int32_t cy) {
    (void)win;
    if (fm->press_idx < 0 || fm->press_idx >= fm->n) return;
    if (!fm_selection_srfs(fm)) {
        fm_status("拖放仅支持 StarRing OS 卷");
        fm->press_idx = -1;
        gui_damage();
        return;
    }
    if (!fm->selected[fm->press_idx])
        fm_select_only(fm, fm->press_idx);
    if (fm_sel_count(fm) <= 0) return;

    fm->file_drag = true;
    fm->press_idx = -1;
    fm->drag_cx = cx;
    fm->drag_cy = cy;
    fm->drop_valid = false;
    fm->drop_path[0] = 0;
    gui_damage();
}

static void fm_file_drag_update(struct gui_window *win, struct fm *fm,
                                int32_t cx, int32_t cy) {
    int32_t oldx = fm->drag_cx, oldy = fm->drag_cy;
    if (oldx != cx || oldy != cy) {
        int32_t gx, gy; uint32_t gw, gh;
        fm_ghost_rect(fm, oldx, oldy, &gx, &gy, &gw, &gh);
        dmg_rel(win, gx, gy, gw, gh);

        fm->drag_cx = cx;
        fm->drag_cy = cy;

        fm_ghost_rect(fm, cx, cy, &gx, &gy, &gw, &gh);
        dmg_rel(win, gx, gy, gw, gh);
    }

    char nd[FS_PATH_MAX];
    bool nv = fm_drop_target(win, fm, cx, cy, nd);
    bool changed = (nv != fm->drop_valid) ||
                   (nv && kstrcmp(nd, fm->drop_path));
    if (changed) {
        fm->drop_valid = nv;
        if (nv) path_copy(fm->drop_path, nd);
        else    fm->drop_path[0] = 0;
        gui_damage();       /* 目标文件夹/导航项高亮变化 */
    }
}

static void fm_file_drag_end(struct gui_window *win, struct fm *fm,
                             int32_t cx, int32_t cy) {
    fm_file_drag_update(win, fm, cx, cy);

    bool had_drop = fm->drop_valid;
    bool copy = keyboard_ctrl_down();
    if (had_drop)
        fm_xfer_begin(fm, fm->drop_path, !copy, false);

    fm->file_drag = false;
    fm->drop_valid = false;
    fm->drop_path[0] = 0;
    fm->press_idx = -1;
    fm->drag_cx = cx;
    fm->drag_cy = cy;
    gui_damage();
}

static void fm_drag_begin(struct gui_window *win, struct fm *fm,
                          int32_t cx, int32_t cy) {
    struct layout L;
    layout_calc(win, fm, &L);
    if (L.grid_h == 0) return;
    if (cx < L.mx || cx >= L.mx + (int32_t)L.mw ||
        cy < L.grid_y || cy >= L.grid_y + (int32_t)L.grid_h) return;
    if (grid_hit_idx(fm, &L, cx, cy) >= 0) return;

    fm->drag_active = true;
    fm->drag_ctrl = keyboard_ctrl_down();
    fm->drag_moved = false;
    fm->drag_x0 = cx;
    fm->drag_y0 = cy;
    fm->drag_x1 = cx;
    fm->drag_y1 = cy;

    if (!fm->drag_ctrl) {
        fm_clear_selection(fm);
        fm->sel = -1;
        dmg_grid(win, fm);
    }
}

static void fm_drag_update(struct gui_window *win, struct fm *fm,
                           int32_t cx, int32_t cy) {
    struct layout L;
    layout_calc(win, fm, &L);
    int32_t maxx = L.mx + (int32_t)L.mw - 1;
    int32_t maxy = L.grid_y + (int32_t)L.grid_h - 1;
    if (cx < L.mx) cx = L.mx;
    if (cx > maxx) cx = maxx;
    if (cy < L.grid_y) cy = L.grid_y;
    if (cy > maxy) cy = maxy;

    int32_t ox0 = fm->drag_x0, oy0 = fm->drag_y0;
    int32_t ox1 = fm->drag_x1, oy1 = fm->drag_y1;
    if (cx == ox1 && cy == oy1) return;
    fm->drag_x1 = cx;
    fm->drag_y1 = cy;
    if (cx < ox0 - 2 || cx > ox0 + 2 || cy < oy0 - 2 || cy > oy0 + 2)
        fm->drag_moved = true;

    /* 旧框和新框的并集：一次局部重绘擦掉旧线并画新线。 */
    int32_t x0 = ox0 < ox1 ? ox0 : ox1;
    int32_t y0 = oy0 < oy1 ? oy0 : oy1;
    int32_t x1 = ox0 > ox1 ? ox0 : ox1;
    int32_t y1 = oy0 > oy1 ? oy0 : oy1;
    int32_t nx0 = fm->drag_x0 < cx ? fm->drag_x0 : cx;
    int32_t ny0 = fm->drag_y0 < cy ? fm->drag_y0 : cy;
    int32_t nx1 = fm->drag_x0 > cx ? fm->drag_x0 : cx;
    int32_t ny1 = fm->drag_y0 > cy ? fm->drag_y0 : cy;
    if (nx0 < x0) x0 = nx0;
    if (ny0 < y0) y0 = ny0;
    if (nx1 > x1) x1 = nx1;
    if (ny1 > y1) y1 = ny1;
    dmg_box(win, x0, y0, x1, y1);
}

static void fm_drag_end(struct gui_window *win, struct fm *fm,
                        int32_t cx, int32_t cy) {
    fm_drag_update(win, fm, cx, cy);

    int32_t x0 = fm->drag_x0, y0 = fm->drag_y0;
    int32_t x1 = fm->drag_x1, y1 = fm->drag_y1;
    if (x1 < x0) { int32_t t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int32_t t = y0; y0 = y1; y1 = t; }
    dmg_box(win, x0, y0, x1, y1);

    if (x1 - x0 >= 2 && y1 - y0 >= 2) {
        struct layout L;
        layout_calc(win, fm, &L);
        int first = -1;
        for (int i = 0; i < fm->n; i++) {
            int32_t tx, ty;
            if (!tile_dx(fm, &L, i, &tx, &ty)) continue;
            int32_t ccx = tx + (int32_t)TILE_W / 2;
            int32_t ccy = ty + (int32_t)TILE_H / 2;
            if (ccx < x0 || ccx > x1 || ccy < y0 || ccy > y1) continue;
            if (i < FS_MAX_DIRENTS) fm->selected[i] = true;
            if (first < 0) first = i;
        }
        if (first >= 0) fm->sel = first;
    }

    fm->drag_active = false;
    dmg_grid(win, fm);
}

static void fm_drag(struct gui_window *win, int32_t cx, int32_t cy,
                    bool pressed) {
    struct fm *fm = win->data;
    if (!fm) return;

    if (fm->file_drag) {
        if (pressed) fm_file_drag_update(win, fm, cx, cy);
        else         fm_file_drag_end(win, fm, cx, cy);
        return;
    }

    if (!fm->drag_active) {
        if (pressed && fm->press_idx >= 0) {
            int32_t dx = cx - fm->press_x;
            int32_t dy = cy - fm->press_y;
            if (dx > 4 || dx < -4 || dy > 4 || dy < -4)
                fm_file_drag_begin(win, fm, cx, cy);
        }
        return;
    }

    if (pressed) fm_drag_update(win, fm, cx, cy);
    else         fm_drag_end(win, fm, cx, cy);
}

/* ---------------- 交互 ---------------- */
static bool cmd_enabled(struct fm *fm, int i) {
    switch (i) {
    case CB_NEW:     return !fm->viewing && path_on_srfs(fm->path);
    case CB_DEL:     return !fm->viewing && fm_selection_srfs(fm);
    case CB_REFRESH: return !fm->viewing;
    default:         return false;
    }
}

static void fm_click(struct gui_window *win, int32_t cx, int32_t cy, bool dbl) {
    struct fm *fm = win->data;
    fm->press_idx = -1;

    /* 属性框是模态的：点 Close 或框外关闭。 */
    if (fm->props_open) {
        int32_t bx, by; uint32_t bw, bh;
        props_close_rect(win, &bx, &by, &bw, &bh);
        int32_t px, py; uint32_t pw, ph;
        props_rect(win, &px, &py, &pw, &ph);
        if (gui_hit(cx, cy, bx, by, bw, bh) ||
            !gui_hit(cx, cy, px, py, pw, ph))
            fm_close_props(fm);
        return;
    }

    if (fm->prompt || fm->confirm_del >= 0) return;

    /* 命令栏 */
    if (cy < CMD_H) {
        for (int i = 0; i < CB_N; i++) {
            if (!gui_hit(cx, cy, cbtn_dx(i), CB_DY, cb_w[i], CB_H)) continue;
            if (!cmd_enabled(fm, i)) return;
            switch (i) {
            case CB_NEW:     fm_start_prompt(fm, FS_TYPE_DIR); break;
            case CB_DEL:     fm_start_delete(fm); break;
            case CB_REFRESH: fm_load(fm); gui_damage(); break;
            }
            return;
        }
        return;
    }

    /* 地址栏 */
    if (cy < CMD_H + ADDR_H) {
        if (fm->viewing) return;
        for (int i = 0; i < AB_N; i++) {
            if (!gui_hit(cx, cy, abtn_dx(i), abtn_dy(), ABTN_SZ, ABTN_SZ))
                continue;
            if (i == AB_BACK)      fm_back(fm);
            else if (i == AB_FWD)  fm_fwd(fm);
            else if (i == AB_UP)   fm_up(fm);
            return;
        }
        return;
    }

    if (fm->viewing) return;

    int32_t by = body_dy();
    if (cy >= (int32_t)(by + fm_body_h(win))) return;    /* 状态栏 */

    /* 导航窗格 */
    if (cx < NAV_W) {
        for (int i = 0; i < fm->side_n; i++) {
            int32_t y = side_dy(fm, i);
            if (y < 0) continue;
            if (cy >= y && cy < y + SIDE_ROW_H) {
                fm_nav(fm, fm->side[i].path, true);
                return;
            }
        }
        return;
    }

    struct layout L;
    layout_calc(win, fm, &L);

    /* 分组标题 → 折叠/展开 */
    if (cy >= L.sec_y && cy < L.sec_y + SEC_H) {
        fm->folders_open = !fm->folders_open;
        fm->htile = -1;
        gui_damage();
        return;
    }
    if (L.drv_y && cy >= L.drv_y && cy < L.drv_y + SEC_H) {
        fm->drives_open = !fm->drives_open;
        gui_damage();
        return;
    }
    if (L.drv_y && cy >= L.drv_y + SEC_H &&
        cy < L.drv_y + SEC_H + (int32_t)L.drv_h) {
        int di = (cy - (L.drv_y + SEC_H)) / DRIVE_H;
        if (dbl && di >= 0 && di < fm->mount_n)
            fm_nav(fm, fm->mounts[di].path, true);
        return;
    }

    /* 大图标网格 */
    if (cx < L.mx || L.grid_h == 0) return;
    if (cy < L.grid_y || cy >= L.grid_y + (int32_t)L.grid_h) return;

    int idx = grid_hit_idx(fm, &L, cx, cy);
    if (idx < 0) {
        /* 空白处：开始橡皮筋框选；Ctrl 时保留已有选择。 */
        bool ctrl = keyboard_ctrl_down();
        if (!ctrl) {
            fm_clear_selection(fm);
            fm->sel = -1;
            dmg_grid(win, fm);
        }
        fm_drag_begin(win, fm, cx, cy);
        return;
    }

    if (keyboard_ctrl_down()) {
        fm_toggle_selected(fm, idx);
        dmg_tile(win, fm, idx);
        dmg_chrome(win);
        return;
    }

    if (fm_sel_count(fm) != 1 || !fm->selected[idx]) {
        fm_select_only(fm, idx);
        dmg_grid(win, fm);
    } else {
        fm->sel = idx;
    }
    if (dbl) {
        fm_open_selected(fm);
    } else {
        fm->press_idx = idx;
        fm->press_x = cx;
        fm->press_y = cy;
    }
}

/* ---------------- 右键菜单 ---------------- */

/* 菜单数组是单例的（同一时刻只有一个上下文菜单）。NULL = 分隔线。 */
static const char *g_item_ctx[GUI_CTX_MAX_ITEMS];
static uint8_t     g_item_ctx_flags[GUI_CTX_MAX_ITEMS];
static int         g_item_ctx_n;

static const char *g_bg_ctx[GUI_CTX_MAX_ITEMS];
static uint8_t     g_bg_ctx_flags[GUI_CTX_MAX_ITEMS];
static int         g_bg_ctx_n;

static void fm_ctx_pick(int index, void *user);

static void fm_open_item_ctx(struct fm *fm, int idx, int32_t sx, int32_t sy) {
    bool can_mod = path_on_srfs(fm->path);
    bool can_clip = can_mod && fm_selection_srfs(fm);
    bool single = (fm_sel_count(fm) == 1);
    bool is_dir = (idx >= 0 && idx < fm->n &&
                   fm->ents[idx].type == FS_TYPE_DIR);

    bool can_paste = false;
    if (single && is_dir && g_clip_n > 0) {
        char dest[FS_PATH_MAX];
        path_join(dest, fm->path, fm->ents[idx].name);
        can_paste = path_on_srfs(dest);
    }

    int n = 0;
    g_item_ctx[n] = "打开";       g_item_ctx_flags[n++] = 0;
    g_item_ctx[n] = "复制";       g_item_ctx_flags[n++] = can_clip ? 0 : GUI_CTX_DISABLED;
    g_item_ctx[n] = "剪切";        g_item_ctx_flags[n++] = can_clip ? 0 : GUI_CTX_DISABLED;
    g_item_ctx[n] = "粘贴";      g_item_ctx_flags[n++] = can_paste ? 0 : GUI_CTX_DISABLED;
    g_item_ctx[n] = 0;            g_item_ctx_flags[n++] = 0;
    g_item_ctx[n] = "重命名";     g_item_ctx_flags[n++] = (can_mod && single) ? 0 : GUI_CTX_DISABLED;
    g_item_ctx[n] = "删除";     g_item_ctx_flags[n++] = can_clip ? 0 : GUI_CTX_DISABLED;
    g_item_ctx[n] = "属性"; g_item_ctx_flags[n++] = 0;
    g_item_ctx_n = n;

    gui_context_open_ex(sx, sy, g_item_ctx, g_item_ctx_flags, g_item_ctx_n,
                        fm_ctx_pick, fm);
}

static void fm_open_bg_ctx(struct fm *fm, int32_t sx, int32_t sy) {
    bool can_mod = path_on_srfs(fm->path);
    bool can_paste = can_mod && g_clip_n > 0;

    int n = 0;
    g_bg_ctx[n] = "粘贴";             g_bg_ctx_flags[n++] = can_paste ? 0 : GUI_CTX_DISABLED;
    g_bg_ctx[n] = 0;                   g_bg_ctx_flags[n++] = 0;
    g_bg_ctx[n] = "新建文件夹";        g_bg_ctx_flags[n++] = can_mod ? 0 : GUI_CTX_DISABLED;
    g_bg_ctx[n] = "新建文件";          g_bg_ctx_flags[n++] = can_mod ? 0 : GUI_CTX_DISABLED;
    g_bg_ctx[n] = 0;                   g_bg_ctx_flags[n++] = 0;
    g_bg_ctx[n] = "全选";        g_bg_ctx_flags[n++] = fm->n > 0 ? 0 : GUI_CTX_DISABLED;
    g_bg_ctx[n] = "刷新";           g_bg_ctx_flags[n++] = 0;
    g_bg_ctx[n] = "在此打开终端"; g_bg_ctx_flags[n++] = 0;
    g_bg_ctx_n = n;

    gui_context_open_ex(sx, sy, g_bg_ctx, g_bg_ctx_flags, g_bg_ctx_n,
                        fm_ctx_pick, fm);
}

/* 菜单选中回调（PS/2 中断上下文）：只做状态变更 + gui_damage* */
static void fm_ctx_pick(int index, void *user) {
    struct fm *fm = (struct fm *)user;
    if (!fm || !fm->win) return;

    if (fm->ctx_mode == 1) {
        fm->sel = fm->ctx_tile;            /* 菜单期间选中项不会变，这里兜底 */
        if (index == 0) {
            fm_open_selected(fm);                              /* Open */
        } else if (index == 1) {
            fm_clip_set(fm, false);                            /* Copy */
        } else if (index == 2) {
            fm_clip_set(fm, true);                             /* Cut */
        } else if (index == 3) {
            if (fm->ctx_tile >= 0 && fm->ctx_tile < fm->n &&
                fm->ents[fm->ctx_tile].type == FS_TYPE_DIR) {
                char dest[FS_PATH_MAX];
                path_join(dest, fm->path, fm->ents[fm->ctx_tile].name);
                fm_xfer_begin(fm, dest, g_clip_cut, true);     /* Paste into folder */
            }
        } else if (index == 5) {
            fm_start_rename(fm, fm->ctx_tile);                 /* Rename */
        } else if (index == 6) {
            fm_start_delete(fm);                               /* Delete */
        } else if (index == 7) {
            fm_open_props(fm, fm->ctx_tile);                   /* Properties */
        }
    } else if (fm->ctx_mode == 2) {
        if (index == 0) {
            fm_xfer_begin(fm, fm->path, g_clip_cut, true);     /* Paste */
        } else if (index == 2) {
            fm_start_prompt(fm, FS_TYPE_DIR);                  /* New Folder */
        } else if (index == 3) {
            fm_start_prompt(fm, FS_TYPE_FILE);                 /* New File */
        } else if (index == 5) {
            fm_select_all(fm);                                 /* Select All */
            gui_damage();
        } else if (index == 6) {
            fm_load(fm); gui_damage();                         /* Refresh */
        } else if (index == 7) {
            terminal_open_at(fm->path);                        /* Open Terminal Here */
        }
    }
    fm->ctx_mode = 0;
}

static void fm_rclick(struct gui_window *win, int32_t cx, int32_t cy) {
    struct fm *fm = win->data;
    if (!fm || fm->prompt || fm->confirm_del >= 0 || fm->viewing) return;
    if (fm->props_open || fm->file_drag) return;

    /* 命令栏 / 地址栏 / 导航窗格：不弹菜单 */
    if (cy < CMD_H + ADDR_H) return;
    if (cx < NAV_W) return;

    int32_t sx = gui_content_x(win) + cx;
    int32_t sy = gui_content_y(win) + cy;

    struct layout L;
    layout_calc(win, fm, &L);

    /* 命中大图标网格里的某一项？ */
    int idx = grid_hit_idx(fm, &L, cx, cy);

    if (idx >= 0) {
        /* 右键已选中的项：保留整组多选；否则只选中这一项。 */
        if (!fm->selected[idx] || fm_sel_count(fm) == 0) {
            fm_select_only(fm, idx);
            dmg_grid(win, fm);
        } else {
            fm->sel = idx;
        }
        fm->ctx_mode = 1;
        fm->ctx_tile = idx;
        fm_open_item_ctx(fm, idx, sx, sy);
    } else {
        fm_clear_selection(fm);
        fm->sel = -1;
        dmg_grid(win, fm);
        fm->ctx_mode = 2;
        fm->ctx_tile = -1;
        fm_open_bg_ctx(fm, sx, sy);
    }
}

static void fm_hover(struct gui_window *win, int32_t cx, int32_t cy) {
    struct fm *fm = win->data;
    if (g_status[0] && timer_ticks() - g_status_tick >= FM_STATUS_TICKS) {
        g_status[0] = 0;
        gui_damage();               /* 让过期提示在下次鼠标移动时消失 */
    }
    if (fm->props_open || fm->drag_active || fm->file_drag) return;
    int  hcmd = -1, hab = -1, hside = -1, htile = -1, hsec = HS_NONE;
    int  hdrive = -1;

    if (cx >= 0 && cy >= 0 && !fm->viewing &&
        !fm->prompt && fm->confirm_del < 0) {
        if (cy < CMD_H) {
            for (int i = 0; i < CB_N; i++)
                if (gui_hit(cx, cy, cbtn_dx(i), CB_DY, cb_w[i], CB_H)) {
                    if (cmd_enabled(fm, i)) hcmd = i;
                    break;
                }
        } else if (cy < CMD_H + ADDR_H) {
            for (int i = 0; i < AB_N; i++)
                if (gui_hit(cx, cy, abtn_dx(i), abtn_dy(), ABTN_SZ, ABTN_SZ)) {
                    hab = i;
                    break;
                }
        } else if (cy < (int32_t)(body_dy() + fm_body_h(win))) {
            struct layout L;
            layout_calc(win, fm, &L);
            if (cx < NAV_W) {
                for (int i = 0; i < fm->side_n; i++) {
                    int32_t y = side_dy(fm, i);
                    if (y >= 0 && cy >= y && cy < y + SIDE_ROW_H) {
                        hside = i;
                        break;
                    }
                }
            } else if (cy >= L.sec_y && cy < L.sec_y + SEC_H) {
                hsec = HS_FOLDERS;
            } else if (L.drv_y && cy >= L.drv_y && cy < L.drv_y + SEC_H) {
                hsec = HS_DRIVES;
            } else if (L.drv_y && cy >= L.drv_y + SEC_H &&
                       cy < L.drv_y + SEC_H + (int32_t)L.drv_h) {
                if (fm->drives_open) {
                    int di = (cy - (L.drv_y + SEC_H)) / DRIVE_H;
                    if (di >= 0 && di < fm->mount_n) hdrive = di;
                }
            } else if (L.grid_h > 0 && cx >= L.mx &&
                       cy >= L.grid_y && cy < L.grid_y + (int32_t)L.grid_h) {
                int col = (cx - L.mx) / (int32_t)TILE_W;
                int row = (cy - L.grid_y) / (int32_t)TILE_H;
                if (col < L.cols && row < L.rows) {
                    int idx = fm->top + row * L.cols + col;
                    if (idx >= 0 && idx < fm->n) htile = idx;
                }
            }
        }
    }

    if (hcmd != fm->hcmd) {
        dmg_cmd(win, fm->hcmd);
        dmg_cmd(win, hcmd);
        fm->hcmd = hcmd;
    }
    if (hab != fm->hab) {
        dmg_ab(win, fm->hab);
        dmg_ab(win, hab);
        fm->hab = hab;
    }
    if (hside != fm->hside) {
        dmg_side(win, fm, fm->hside);
        dmg_side(win, fm, hside);
        fm->hside = hside;
    }
    if (htile != fm->htile) {
        dmg_tile(win, fm, fm->htile);
        dmg_tile(win, fm, htile);
        fm->htile = htile;
    }
    if (hdrive != fm->hdrive) {
        fm->hdrive = hdrive;
        if (fm->win) gui_damage();     /* 驱动器行区域较小，直接整体重绘 */
    }
    if (hsec != fm->hsec) {
        fm->hsec = hsec;
        gui_damage();
    }
}

static void fm_grid_move(struct fm *fm, struct gui_window *win, int dx, int dy) {
    struct layout L;
    layout_calc(win, fm, &L);
    if (fm->n == 0) return;
    int cols = L.cols > 0 ? L.cols : 1;
    int idx = fm->sel;
    if (idx < 0) idx = 0;

    if (dy) {
        idx += dy * cols;
        if (idx < 0) idx = 0;
        if (idx >= fm->n) idx = fm->n - 1;
    }
    if (dx) {
        int col = idx % cols;
        if (dx < 0) { if (col > 0) idx--; }
        else        { if (col + 1 < cols && idx + 1 < fm->n) idx++; }
    }

    fm_select_only(fm, idx);
    fm_ensure_visible(fm, &L);
    dmg_grid(win, fm);
}

static void fm_key(struct gui_window *win, uint16_t key) {
    struct fm *fm = win->data;

    if (fm->props_open) {
        if (key == KEY_ESC || key == '\n' || key == ' ')
            fm_close_props(fm);
        return;
    }

    /* 新建文件夹/文件：输入名称 */
    if (fm->prompt) {
        if (key == '\n') { fm_commit_prompt(fm); return; }
        if (key == KEY_ESC) {
            fm->prompt = false;
            fm->prompt_action = FM_PROMPT_NEW;
            fm->rename_idx = -1;
            fm->plen = 0;
            fm->pbuf[0] = 0;
            gui_damage();
            return;
        }
        if (key == '\b') {
            if (fm->plen > 0) fm->pbuf[--fm->plen] = 0;
            gui_damage();
            return;
        }
        if (key >= 32 && key < 127 && key != '/' && fm->plen < FS_NAME_MAX) {
            fm->pbuf[fm->plen++] = (char)key;
            fm->pbuf[fm->plen] = 0;
            gui_damage();
        }
        return;
    }

    /* 删除确认 */
    if (fm->confirm_del >= 0) {
        if (key == 'y' || key == 'Y' || key == '\n') { fm_commit_delete(fm); return; }
        if (key == 'n' || key == 'N' || key == KEY_ESC) {
            fm->confirm_del = -1;
            gui_damage();
        }
        return;
    }

    if (fm->viewing) {
        switch (key) {
        case KEY_UP:   if (fm->vscroll) fm->vscroll--; break;
        case KEY_DOWN: fm->vscroll++; break;
        case KEY_PGUP: fm->vscroll = (fm->vscroll > 10) ? fm->vscroll - 10 : 0; break;
        case KEY_PGDN: fm->vscroll += 10; break;
        case KEY_HOME: fm->vscroll = 0; break;
        case KEY_ESC:
        case '\b':
            fm->viewing = false;
            gui_window_set_title(win, path_base(fm->path));
            break;
        default:
            return;
        }
        gui_damage();
        return;
    }

    switch (key) {
    case KEY_UP:    fm_grid_move(fm, win, 0, -1); return;
    case KEY_DOWN:  fm_grid_move(fm, win, 0,  1); return;
    case KEY_LEFT:  fm_grid_move(fm, win, -1, 0); return;
    case KEY_RIGHT: fm_grid_move(fm, win,  1, 0); return;
    case KEY_PGUP:
        fm->sel = (fm->sel > 8) ? fm->sel - 8 : 0;
        fm_select_only(fm, fm->sel);
        break;
    case KEY_PGDN:
        fm->sel = (fm->sel < 0 ? 0 : fm->sel) + 8;
        if (fm->sel >= fm->n) fm->sel = fm->n ? fm->n - 1 : -1;
        fm_select_only(fm, fm->sel);
        break;
    case KEY_HOME:
        fm_select_only(fm, fm->n ? 0 : -1);
        break;
    case KEY_END:
        fm_select_only(fm, fm->n ? fm->n - 1 : -1);
        break;
    case '\n':     fm_open_selected(fm); return;
    case '\b':     fm_up(fm); return;
    case 'n':
    case 'N':      fm_start_prompt(fm, FS_TYPE_DIR); return;
    case KEY_DELETE: fm_start_delete(fm); return;
    case 'c':
    case 'C':
        if (keyboard_ctrl_down()) fm_clip_set(fm, false);
        return;
    case 'x':
    case 'X':
        if (keyboard_ctrl_down()) fm_clip_set(fm, true);
        return;
    case 'v':
    case 'V':
        if (keyboard_ctrl_down())
            fm_xfer_begin(fm, fm->path, g_clip_cut, true);
        return;
    case 'a':
    case 'A':
        if (!keyboard_ctrl_down()) return;
        fm_select_all(fm);
        dmg_grid(win, fm);
        return;
    case 'r':
    case 'R':      fm_load(fm); break;
    default:
        return;
    }
    dmg_grid(win, fm);
}

static void fm_close(struct gui_window *win) {
    struct fm *fm = win->data;
    if (fm) {
        fm->win = 0;
        fm->ctx_mode = 0;
        fm->drag_active = false;
        fm->file_drag = false;
        fm->drop_valid = false;
        fm->press_idx = -1;
        fm->props_open = false;
    }
    /* 窗口关了，挂在它上面的右键菜单也要收起，避免回调打到已关闭的实例 */
    if (gui_context_active()) gui_context_close();
}

/* ---------------- 对外接口 ---------------- */
void files_open(const char *path) {
    struct fm *fm = 0;
    for (int i = 0; i < FM_MAX; i++)
        if (!fms[i].win) { fm = &fms[i]; break; }
    if (!fm) return;

    memset(fm, 0, sizeof(*fm));
    fm->hcmd = -1;
    fm->hab = -1;
    fm->hside = -1;
    fm->htile = -1;
    fm->hsec = HS_NONE;
    fm->hdrive = -1;
    fm->confirm_del = -1;
    fm->del_count = 0;
    fm->sel = -1;
    fm->press_idx = -1;
    fm->props_idx = -1;
    fm->prompt_action = FM_PROMPT_NEW;
    fm->rename_idx = -1;
    fm->prompt_type = FS_TYPE_DIR;
    fm->folders_open = true;
    fm->drives_open = true;
    {
        /* 有界拷贝：files_open 的路径来自外部（shell/GUI 图标） */
        const char *src = (path && path[0]) ? path : "/";
        size_t pn = 0;
        while (src[pn] && pn + 1 < FS_PATH_MAX) { fm->path[pn] = src[pn]; pn++; }
        fm->path[pn] = 0;
    }

    /* 与屏幕尺寸适配，避免小分辨率下超出 */
    uint32_t w = FM_W, h = FM_H;
    uint32_t sw = gfx_width(), sh = gfx_height();
    if (sw > 80 && w > sw - 40) w = sw - 40;
    if (sh > GUI_TASKBAR_H + 80 && h > sh - GUI_TASKBAR_H - 60)
        h = sh - GUI_TASKBAR_H - 60;

    int open_count = 0;
    for (int i = 0; i < FM_MAX; i++) if (fms[i].win) open_count++;
    int32_t ox = FM_X + open_count * 26;
    int32_t oy = FM_Y + open_count * 24;
    if (sw > w && ox > (int32_t)(sw - w)) ox = (int32_t)(sw - w);
    if (sh > h && oy > (int32_t)(sh - h)) oy = (int32_t)(sh - h);
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;

    struct gui_window *win = gui_window_open("此电脑", ox, oy, w, h);
    if (!win) return;

    fm->win = win;
    win->data = fm;
    win->draw = fm_draw;
    win->on_click = fm_click;
    win->on_hover = fm_hover;
    win->on_drag = fm_drag;
    win->on_rclick = fm_rclick;
    win->on_key = fm_key;
    win->on_close = fm_close;
    gui_window_set_icon(win, GUI_ICON_PC);

    fm_build_side(fm);
    fm_load(fm);
    gui_window_set_title(win, path_base(fm->path));
    gui_damage();
}

void files_refresh_all(void) {
    for (int i = 0; i < FM_MAX; i++) {
        if (!fms[i].win) continue;
        fm_build_side(&fms[i]);
        if (!fms[i].viewing) fm_load(&fms[i]);
    }
    gui_damage();
}
