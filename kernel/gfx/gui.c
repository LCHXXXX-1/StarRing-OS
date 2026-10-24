#include "gui.h"
#include "gfx.h"
#include "font.h"
#include "font_ttf.h"
#include "console.h"
#include "drivers/framebuffer.h"
#include "drivers/timer.h"
#include "boot/sysinit.h"      /* 开机遮罩里已读好的 RTC + 渐变壁纸缓存 */
#include "drivers/power.h"
#include "drivers/keyboard.h"
#include "drivers/input.h"
#include "arch/x86_64/cpu.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "apps/terminal.h"
#include "apps/files.h"
#include "apps/disk_utility.h"
#include "apps/taskmgr.h"
#include "apps/sysmon.h"
#include "apps/sysprop.h"
#include "task/task.h"

/* ---------------- 桌面布局常量 ---------------- */
#define START_W      100
#define START_H      26
#define START_PAD    5

#define MENU_W       216
#define MENU_HDR_H   42
#define MENU_ITEM_H  32
#define MENU_SEP_H   11
#define MENU_PAD     6

#define ICON_W       46
#define ICON_H       44
#define ICON_X       30
#define ICON_Y0      26
#define ICON_DY      78

#define DBL_TICKS    45        /* 双击判定窗口（0.45s @100Hz） */

/* ---------------- 窗口状态 ---------------- */
static struct gui_window  wins[GUI_MAX_WINDOWS];
static struct gui_window *zorder[GUI_MAX_WINDOWS];
static int                zcount = 0;
static struct gui_window *focused = 0;

/* ---------------- 鼠标指针 ---------------- */
#define ARROW_W 12
#define ARROW_H 16
static const char *arrow[ARROW_H] = {
    "k...........",
    "kk..........",
    "kwk.........",
    "kwwk........",
    "kwwwk.......",
    "kwwwwk......",
    "kwwwwwk.....",
    "kwwwwwwk....",
    "kwwwwwwwk...",
    "kwwwwwwwwk..",
    "kwwwwwwwwwk.",
    "kwwwwwkkkkkk",
    "kwkwwk......",
    "kk.kwwk.....",
    "k...kwwk....",
    ".....kkkk...",
};

static int32_t mx = 0, my = 0;
static int     prev_left = 0;
static int     prev_right = 0;
static int     pshown = 0;
static int32_t pshown_x = 0, pshown_y = 0;
static uint32_t pbackup[ARROW_W * ARROW_H];

/* ---------------- 交互状态 ---------------- */
static int      dragging = 0;
static struct gui_window *drag_win = 0;
static int32_t  grab_dx = 0, grab_dy = 0;

/* 应用内容区左键拖动（资源管理器框选） */
static struct gui_window *app_drag_win = 0;

/* CPU 空闲统计：gui_run() 进入 hlt 前置位，PIT 中断在 hlt 中到来时计数 */
static volatile int      idle_waiting = 0;
static volatile uint64_t idle_ticks = 0;
static void (*slow_tick_fn)(uint64_t ticks) = 0;

/* 全屏登录 overlay 钩子（apps/login.c 安装）。全为 0 时所有分流都不生效。 */
static void (*login_draw_fn)(void) = 0;
static void (*login_mouse_fn)(uint8_t buttons, int32_t dx, int32_t dy) = 0;
static void (*login_key_fn)(uint16_t key) = 0;
static void (*login_tick_fn)(uint64_t ticks) = 0;

static struct gui_window *last_click_win = 0;
static uint64_t last_click_tick = 0;

static int      desk_sel = -1;
static int      last_icon = -1;
static uint64_t last_icon_tick = 0;

static int      start_open = 0;
static int      start_hover = 0;
static int      menu_hover = -1;

/* 悬停状态：只在“状态发生变化”时请求重绘，普通移动走指针快速路径 */
static struct gui_window *hover_win = 0;
static int      hover_close = 0;
static int      hover_maxbtn = 0;
static int      hover_task = -1;
static int      hover_icon = -1;

static volatile int need_composite = 0;
static volatile int in_composite = 0;

/* 局部重绘：损坏矩形（并集）。need_composite 为“整屏重绘”标志 */
static volatile int      dmg_able = 0;
static volatile int32_t  dmg_x0 = 0, dmg_y0 = 0, dmg_x1 = 0, dmg_y1 = 0;

/* 当前合成的裁剪区（与 gfx_clip_set 的矩形一致）。
   desktop_draw 里的每个绘制元素先做一次矩形粗筛：屏幕外的窗口/图标/整条
   任务栏直接跳过，省掉 console_render/TTF 的字形查找与逐像素混合——
   这是拖动窗口时最大的开销。comp_valid=0 时表示不裁剪（整屏合成）。 */
static int32_t  comp_x0 = 0, comp_y0 = 0;
static uint32_t comp_w = 0, comp_h = 0;
static int      comp_valid = 0;

/* 矩形是否与当前合成裁剪区相交；没有裁剪区时一律返回 true */
static bool rect_hits_clip(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (!comp_valid) return true;
    if (w == 0 || h == 0) return false;
    if (x >= comp_x0 + (int32_t)comp_w) return false;
    if (y >= comp_y0 + (int32_t)comp_h) return false;
    if (x + (int32_t)w <= comp_x0) return false;
    if (y + (int32_t)h <= comp_y0) return false;
    return true;
}

static int clk_h = -1, clk_m = 0, clk_s = 0;
static int clk_day = 0, clk_mon = 0, clk_year = 0;

/* 局部脏区（在下面各节定义） */
static void dmg_title(const struct gui_window *w);
static void dmg_window(const struct gui_window *w);
static void dmg_task_btn(int idx);
static void dmg_icon(int idx);
static void dmg_start_btn(void);
static void dmg_menu(void);

/* ---------------- 小工具 ---------------- */
static void copy_str(char *dst, const char *src, size_t max) {
    size_t i = 0;
    if (max == 0) return;
    if (!src) { dst[0] = 0; return; }
    while (src[i] && i + 1 < max) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static uint32_t mix(uint32_t a, uint32_t b, int t, int tmax) {
    if (tmax <= 0) return a;
    if (t < 0) t = 0;
    if (t > tmax) t = tmax;
    int ar = (int)((a >> 16) & 0xFF), ag = (int)((a >> 8) & 0xFF), ab = (int)(a & 0xFF);
    int br = (int)((b >> 16) & 0xFF), bg = (int)((b >> 8) & 0xFF), bb = (int)(b & 0xFF);
    int r = ar + (br - ar) * t / tmax;
    int g = ag + (bg - ag) * t / tmax;
    int bl = ab + (bb - ab) * t / tmax;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

static uint32_t wall_color(uint32_t y) {
    uint32_t sh = gfx_height();
    uint32_t mid;
    if (sh == 0) sh = 1;
    if (y >= sh) y = sh - 1;
    mid = sh / 2;
    if (y < mid) return mix(GUI_COL_DESKTOP_TOP, GUI_COL_DESKTOP_MID,
                            (int)y, (int)mid);
    return mix(GUI_COL_DESKTOP_MID, GUI_COL_DESKTOP_BOT,
               (int)(y - mid), (int)(sh - mid));
}

/* 取第 y 行壁纸色：优先读开机遮罩算好的缓存，缓存无效才现场算（冷路径）。
   y 越界一律钳到有效区间，绝不读出 bg_gradient_cache 之外。 */
static uint32_t wall_at(uint32_t y) {
    if (bg_cache_valid()) {
        uint32_t rows = bg_cache_rows;
        if (rows == 0) return wall_color(y);
        if (y >= rows) y = rows - 1;
        return bg_gradient_cache[y];
    }
    return wall_color(y);
}

static uint32_t taskbar_col(int32_t y) {
    int32_t ty = (int32_t)gfx_height() - GUI_TASKBAR_H;
    int t = y - ty;
    if (t < 0) t = 0;
    if (t >= GUI_TASKBAR_H) t = GUI_TASKBAR_H - 1;
    return mix(GUI_COL_TASKBAR_TOP, GUI_COL_TASKBAR_BOT, t, GUI_TASKBAR_H - 1);
}

/* ---------------- 绘制原语 ---------------- */
void gui_fill(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color) {
    gfx_rect(x, y, w, h, color);
}

void gui_pixel(int32_t x, int32_t y, uint32_t color) {
    gfx_pixel(x, y, color);
}

void gui_frame(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (w == 0 || h == 0) return;
    gfx_rect(x, y, w, 1, color);
    gfx_rect(x, y + (int32_t)h - 1, w, 1, color);
    gfx_rect(x, y, 1, h, color);
    gfx_rect(x + (int32_t)w - 1, y, 1, h, color);
}

/* TTF 就绪判断：所有文本绘制统一走这里。
   false 时 ttf_draw_text 内部只画 ASCII 点阵（中文占双宽空位），
   绝不会在 ssfn_* 里解引用空指针。 */
bool is_ttf_ready(void) {
    return ttf_available();
}

/* 按「显示格」上限截断（不是按字节）：ASCII 1 格、CJK 2 格。
   调用方的 max_chars 沿用旧语义 = 可用像素宽 / GUI_FONT_W，也就是格数，
   于是 16px 的汉字自然占 2 格预算，整行不会超宽；并且绝不在 UTF-8
   多字节序列中间下刀，也不会把半个汉字丢给 ssfn（那是空白/乱码来源）。 */
static int utf8_clip(const char *s, int max_cells, char *buf, int cap) {
    int si = 0, di = 0, cells = 0;
    if (!s || !buf || cap <= 0) return 0;
    while (s[si]) {
        unsigned char c = (unsigned char)s[si];
        int len = 1, w = 1;
        if ((c & 0xE0) == 0xC0)      len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (c >= 0x80) {
            if (len == 1) { si++; continue; }   /* 非法首字节/续字节：丢弃 */
            w = 2;                              /* 非 ASCII 一律按双宽布局 */
        }
        if (cells + w > max_cells) break;
        if (di + len >= cap) break;
        for (int k = 0; k < len; k++) buf[di + k] = s[si + k];
        si += len; di += len; cells += w;
    }
    buf[di] = 0;
    return di;
}

void gui_text(int32_t x, int32_t y, const char *s, uint32_t fg, uint32_t bg) {
    if (!s || !s[0]) return;
    ttf_draw_text(x, y, s, fg, bg);            /* bg == 0 表示透明背景 */
}

void gui_text_bold(int32_t x, int32_t y, const char *s, uint32_t fg, uint32_t bg) {
    if (!s || !s[0]) return;
    /* 第二遍向右偏 1px 叠画，模拟加粗；只有第一遍负责填背景 */
    ttf_draw_text(x, y, s, fg, bg);
    ttf_draw_text(x + 1, y, s, fg, 0);
}

void gui_text_max(int32_t x, int32_t y, const char *s, int max_chars,
                  uint32_t fg, uint32_t bg) {
    char buf[192];
    utf8_clip(s, max_chars, buf, (int)sizeof(buf));
    if (!buf[0]) return;
    ttf_draw_text(x, y, buf, fg, bg);
}

void gui_vgrad(int32_t x, int32_t y, uint32_t w, uint32_t h,
               uint32_t top, uint32_t bot) {
    if (h == 0) return;
    for (uint32_t i = 0; i < h; i++)
        gfx_rect(x, y + (int32_t)i, w, 1,
                 mix(top, bot, (int)i, (int)(h > 1 ? h - 1 : 1)));
}

void gui_round_fill(int32_t x, int32_t y, uint32_t w, uint32_t h,
                    uint32_t color, uint32_t corner_bg) {
    if (w == 0 || h == 0) return;
    gfx_rect(x, y, w, h, color);
    if (w < 3 || h < 3) return;
    gfx_pixel(x, y, corner_bg);
    gfx_pixel(x + (int32_t)w - 1, y, corner_bg);
    gfx_pixel(x, y + (int32_t)h - 1, corner_bg);
    gfx_pixel(x + (int32_t)w - 1, y + (int32_t)h - 1, corner_bg);
}

uint32_t gui_str_width(const char *s) {
    if (!s) return 0;
    return ttf_get_text_width(s);     /* 计量与绘制同源，中文排版不再错位 */
}

bool gui_hit(int32_t px, int32_t py, int32_t x, int32_t y,
             uint32_t w, uint32_t h) {
    return px >= x && py >= y && px < x + (int32_t)w && py < y + (int32_t)h;
}

/* 透明背景的限长文本（用于渐变背景，如标题栏/任务栏） */
static void text_t(int32_t x, int32_t y, const char *s, int max_chars,
                   uint32_t fg) {
    char buf[192];
    utf8_clip(s, max_chars, buf, (int)sizeof(buf));
    if (!buf[0]) return;
    ttf_draw_text(x, y, buf, fg, 0);
}

static void fill_circle(int32_t cx, int32_t cy, int32_t r, uint32_t color) {
    for (int32_t j = -r; j <= r; j++) {
        int32_t span = 0;
        while ((span + 1) * (span + 1) + j * j <= r * r) span++;
        gui_fill(cx - span, cy + j, (uint32_t)(2 * span + 1), 1, color);
    }
}

/* 圆环（gap: 0=顶部缺口 1=右上缺口） */
static void draw_ring(int32_t cx, int32_t cy, uint32_t color, int gap) {
    for (int i = -5; i <= 5; i++)
        for (int j = -5; j <= 5; j++) {
            int d = i * i + j * j;
            if (d < 15 || d > 30) continue;
            if (gap == 0 && j <= -3 && i >= -2 && i <= 2) continue;
            if (gap == 1 && j <= -2 && i >= 1) continue;
            gfx_pixel(cx + i, cy + j, color);
        }
}

/* ---------------- 图标 ---------------- */
void gui_icon_pc(int32_t x, int32_t y) {
    gui_fill(x, y, 46, 32, 0x004E6C90);            /* 显示器外壳 */
    gui_fill(x, y, 46, 1, 0x007CA2CC);             /* 顶部高光 */
    gui_fill(x + 3, y + 3, 40, 23, 0x001B2A3C);    /* 内框 */
    for (int i = 0; i < 21; i++)
        gui_fill(x + 4, y + 4 + i, 38, 1, mix(0x004A9AE0, 0x0015304E, i, 20));
    font_draw_str(x + 8, y + 6, "StarRing OS", 0x00EAF4FF);
    gui_fill(x + 4, y + 21, 38, 5, 0x001E3A5A);    /* 屏内任务栏 */
    gui_fill(x + 6, y + 22, 7, 3, 0x0059B0F5);
    gui_fill(x + 15, y + 22, 11, 3, 0x003A5C80);
    gui_fill(x + 28, y + 22, 11, 3, 0x003A5C80);
    gui_fill(x + 20, y + 32, 6, 5, 0x00384F6B);    /* 支架 */
    gui_fill(x + 11, y + 37, 24, 4, 0x004E6C90);   /* 底座 */
    gui_fill(x + 11, y + 37, 24, 1, 0x007CA2CC);
}

void gui_icon_term(int32_t x, int32_t y) {
    gui_fill(x, y, 46, 36, 0x00304A66);
    gui_fill(x, y, 46, 1, 0x00567FA8);
    gui_fill(x + 2, y + 2, 42, 10, 0x002F6FBF);
    gui_fill(x + 2, y + 2, 42, 1, 0x00599AD8);
    gui_fill(x + 5, y + 5, 4, 4, 0x00E0544A);
    gui_fill(x + 3, y + 13, 40, 21, 0x00081018);
    font_draw_str(x + 6, y + 15, ">", 0x0066FF99);
    font_draw_str(x + 6, y + 26, "_", 0x00C8DCF0);
}

void gui_icon_folder(int32_t x, int32_t y) {
    gui_fill(x, y + 2, 16, 12, 0x00C8942F);
    gui_fill(x + 1, y + 3, 14, 10, 0x00F5C542);
    gui_fill(x, y, 7, 3, 0x00C8942F);
    gui_fill(x + 1, y + 3, 14, 1, 0x00FFE08A);
}

void gui_icon_file(int32_t x, int32_t y) {
    gui_fill(x + 1, y, 11, 15, 0x00E8EEF5);
    gui_fill(x + 2, y + 1, 9, 13, 0x00FFFFFF);
    gui_fill(x + 9, y, 3, 4, 0x00A8B4C4);
    gui_fill(x + 3, y + 5, 7, 1, 0x00A8B4C4);
    gui_fill(x + 3, y + 8, 7, 1, 0x00A8B4C4);
    gui_fill(x + 3, y + 11, 5, 1, 0x00A8B4C4);
}

/* 大号硬盘图标（桌面“Disk Utility”用）：金属外壳 + 盘片 + 读写臂 + 指示灯 */
void gui_icon_disk(int32_t x, int32_t y) {
    gui_fill(x, y, 46, 36, 0x004E6C90);            /* 外壳 */
    gui_fill(x, y, 46, 1, 0x007CA2CC);             /* 顶部高光 */
    gui_fill(x + 2, y + 2, 42, 32, 0x001B2A3C);    /* 面板 */

    fill_circle(x + 23, y + 18, 13, 0x002A4E76);   /* 盘片外圈 */
    fill_circle(x + 23, y + 18, 11, 0x003D6FA8);
    fill_circle(x + 23, y + 18, 7,  0x001E3A5A);
    fill_circle(x + 23, y + 18, 3,  0x00C8DCF0);   /* 主轴 */

    gui_fill(x + 30, y + 11, 8, 3, 0x00A8C4E0);    /* 读写臂 */
    gui_fill(x + 35, y + 14, 3, 7, 0x00A8C4E0);
    gui_fill(x + 32, y + 18, 3, 3, 0x0059B0F5);

    gui_fill(x + 6, y + 28, 4, 4, 0x0066FF99);     /* 指示灯 */
    gfx_pixel(x + 41, y + 5, 0x007CA2CC);          /* 螺丝 */
    gfx_pixel(x + 6,  y + 5, 0x007CA2CC);
}

/* 大号任务管理器图标：深色面板 + 柱状图 */
void gui_icon_tasks(int32_t x, int32_t y) {
    gui_fill(x, y, 46, 36, 0x00304A66);
    gui_fill(x, y, 46, 1, 0x00567FA8);
    gui_fill(x + 2, y + 2, 42, 32, 0x00101828);
    gui_fill(x + 7,  y + 22, 6, 9,  0x0059B0F5);
    gui_fill(x + 17, y + 15, 6, 16, 0x0076C4F7);
    gui_fill(x + 27, y + 8,  6, 23, 0x00A8D8FF);
    gui_fill(x + 4,  y + 31, 38, 1, 0x00A8C4E0);
}

/* 大号内存图标：深色底 + 内存条（PCB/芯片/金手指） */
void gui_icon_mem(int32_t x, int32_t y) {
    gui_fill(x, y, 46, 36, 0x00304A66);
    gui_fill(x, y, 46, 1, 0x00567FA8);
    gui_fill(x + 1, y + 1, 44, 34, 0x00101828);
    gui_fill(x + 4, y + 8, 38, 14, 0x002E7D4F);    /* PCB */
    gui_fill(x + 6, y + 10, 34, 10, 0x001E3A2A);
    for (int i = 0; i < 4; i++)
        gui_fill(x + 8 + i * 8, y + 11, 6, 8, 0x00C8DCF0);
    gui_fill(x + 4, y + 22, 38, 4, 0x00C8942F);    /* 金手指 */
    for (int i = 0; i < 9; i++)
        gfx_pixel(x + 6 + i * 4, y + 24, 0x00101828);
}

void gui_draw_mini_icon(int32_t x, int32_t y, int icon) {
    switch (icon) {
    case GUI_ICON_PC:
        gui_fill(x + 1, y + 1, 14, 11, 0x00A8C4E0);
        gui_fill(x + 2, y + 2, 12, 8, 0x002A7CC4);
        gui_fill(x + 3, y + 3, 10, 6, 0x00132A45);
        gui_fill(x + 4, y + 4, 8, 1, 0x0059B0F5);
        gui_fill(x + 7, y + 12, 2, 2, 0x00A8C4E0);
        gui_fill(x + 4, y + 14, 8, 1, 0x00A8C4E0);
        break;
    case GUI_ICON_TERM:
        gui_fill(x + 1, y + 2, 14, 12, 0x00304A66);
        gui_fill(x + 2, y + 3, 12, 3, 0x002F6FBF);
        gui_fill(x + 2, y + 6, 12, 7, 0x00081018);
        gfx_pixel(x + 3, y + 8, 0x0066FF99);
        gfx_pixel(x + 4, y + 9, 0x0066FF99);
        gfx_pixel(x + 3, y + 10, 0x0066FF99);
        gui_fill(x + 6, y + 10, 4, 1, 0x00C8DCF0);
        break;
    case GUI_ICON_FOLDER:
        gui_fill(x + 1, y + 4, 14, 10, 0x00C8942F);
        gui_fill(x + 2, y + 5, 12, 8, 0x00F5C542);
        gui_fill(x + 1, y + 2, 7, 3, 0x00C8942F);
        break;
    case GUI_ICON_RESTART:
        draw_ring(x + 8, y + 8, 0x00C8DCF0, 1);
        gui_fill(x + 9, y + 2, 4, 1, 0x00C8DCF0);
        gui_fill(x + 9, y + 3, 3, 1, 0x00C8DCF0);
        gui_fill(x + 9, y + 4, 2, 1, 0x00C8DCF0);
        gui_fill(x + 9, y + 5, 1, 1, 0x00C8DCF0);
        break;
    case GUI_ICON_POWER:
        draw_ring(x + 8, y + 9, 0x00FFA79A, 0);
        gui_fill(x + 8, y + 2, 1, 7, 0x00FFA79A);
        break;
    case GUI_ICON_DISK:
        /* 硬盘：银色外壳 + 深色面板 + 盘片圆 + 主轴 + 绿色指示灯 */
        gui_fill(x + 1, y + 3, 14, 10, 0x00A8C4E0);
        gui_fill(x + 2, y + 4, 12,  8, 0x002A4E76);
        gui_fill(x + 3, y + 5, 10,  6, 0x000E1C2E);
        draw_ring(x + 8, y + 8, 0x0059B0F5, 1);
        gui_fill(x + 7, y + 7, 2, 2, 0x00C8DCF0);
        gfx_pixel(x + 13, y + 11, 0x0066FF99);
        break;
    case GUI_ICON_TASKS:
        /* 任务管理器：柱状图 + 基线 */
        gui_fill(x + 2,  y + 11, 3, 3,  0x0059B0F5);
        gui_fill(x + 6,  y + 7,  3, 7,  0x0076C4F7);
        gui_fill(x + 10, y + 3,  3, 11, 0x00A8D8FF);
        gui_fill(x + 1,  y + 14, 14, 1, 0x00A8C4E0);
        break;
    case GUI_ICON_MEM:
        /* 内存条：绿色 PCB + 黑色芯片 + 金手指 */
        gui_fill(x + 1, y + 4, 14, 8, 0x002E7D4F);
        gui_fill(x + 2, y + 5, 12, 6, 0x001E3A2A);
        gui_fill(x + 3, y + 6, 3, 4, 0x00C8DCF0);
        gui_fill(x + 7, y + 6, 3, 4, 0x00C8DCF0);
        gui_fill(x + 11, y + 6, 1, 4, 0x00C8DCF0);
        gui_fill(x + 1, y + 12, 14, 2, 0x00C8942F);
        break;
    case GUI_ICON_USB:
        /* USB 闪存盘：金属插头 + 蓝色盘体 + 指示灯 */
        gui_fill(x + 5, y + 1, 6, 4, 0x00C8DCF0);
        gui_fill(x + 6, y + 2, 4, 2, 0x006C7E91);
        gui_fill(x + 3, y + 5, 10, 9, 0x003E86D8);
        gui_fill(x + 4, y + 6, 8, 7, 0x001E4F82);
        gui_fill(x + 5, y + 8, 6, 1, 0x00A8D8FF);
        gui_fill(x + 5, y + 10, 4, 1, 0x00A8D8FF);
        gfx_pixel(x + 11, y + 6, 0x0066FF99);
        break;
    default:
        break;
    }
}

/* ---------------- 指针（直接画在真实帧缓冲上） ----------------
   虚拟坐标 -> 真实帧缓冲坐标。多数机器 1:1；若 Limine 给了大于
   GFX_MAX 的模式，gfx_present_rect() 会把整个虚拟屏缩放到物理屏，
   指针也必须使用同一套映射，否则会画到错误的像素上。 */
static inline int32_t ptr_fb_x(int32_t x) {
    uint32_t vw = gfx_width();
    uint32_t fw = (uint32_t)fb_width();
    if (!vw || !fw) return x;
    return (int32_t)((uint64_t)(uint32_t)x * fw / vw);
}
static inline int32_t ptr_fb_y(int32_t y) {
    uint32_t vh = gfx_height();
    uint32_t fh = (uint32_t)fb_height();
    if (!vh || !fh) return y;
    return (int32_t)((uint64_t)(uint32_t)y * fh / vh);
}

static inline void fb_write(int32_t x, int32_t y, uint32_t color) {
    fb_putpixel(ptr_fb_x(x), ptr_fb_y(y), color);
}

static inline uint32_t fb_read(int32_t x, int32_t y) {
    return fb_getpixel(ptr_fb_x(x), ptr_fb_y(y));
}

static inline bool ptr_in_bounds(int32_t x, int32_t y) {
    return x >= 0 && y >= 0 &&
           x < (int32_t)gfx_width() && y < (int32_t)gfx_height();
}

static void pointer_hide(void) {
    if (!pshown) return;
    for (int32_t j = 0; j < ARROW_H; j++)
        for (int32_t i = 0; i < ARROW_W; i++) {
            int32_t x = pshown_x + i, y = pshown_y + j;
            if (ptr_in_bounds(x, y))
                fb_write(x, y, pbackup[j * ARROW_W + i]);
        }
    pshown = 0;
}

static void pointer_show(void) {
    if (!fb_get()) return;
    /* 指针靠近屏幕边缘时裁剪绘制，而不是整只隐藏。旧行为在 1080p
       下会让指针消失在任务栏/Start 按钮区域。 */
    if (!ptr_in_bounds(mx, my)) return;

    for (int32_t j = 0; j < ARROW_H; j++)
        for (int32_t i = 0; i < ARROW_W; i++) {
            int32_t x = mx + i, y = my + j;
            if (ptr_in_bounds(x, y))
                pbackup[j * ARROW_W + i] = fb_read(x, y);
        }

    for (int32_t j = 0; j < ARROW_H; j++)
        for (int32_t i = 0; i < ARROW_W; i++) {
            int32_t x = mx + i, y = my + j;
            if (!ptr_in_bounds(x, y)) continue;
            char k = arrow[j][i];
            if (k == 'k')      fb_write(x, y, 0x00000000);
            else if (k == 'w') fb_write(x, y, 0x00FFFFFF);
        }
    pshown = 1; pshown_x = mx; pshown_y = my;
}

/* 指针移动的快速路径：只碰指针矩形，不整屏重绘 */
static void pointer_update(void) {
    pointer_hide();
    pointer_show();
}

/* ---------------- 窗口 ---------------- */
int32_t  gui_content_x(const struct gui_window *w) { return w->x + GUI_BORDER; }
int32_t  gui_content_y(const struct gui_window *w) {
    return w->y + GUI_BORDER + GUI_TITLE_H;
}
uint32_t gui_content_w(const struct gui_window *w) { return w->w - 2 * GUI_BORDER; }
uint32_t gui_content_h(const struct gui_window *w) {
    return w->h - 2 * GUI_BORDER - GUI_TITLE_H;
}

static void z_raise(struct gui_window *w) {
    int idx = -1;
    for (int i = 0; i < zcount; i++) if (zorder[i] == w) { idx = i; break; }
    if (idx < 0) return;
    for (int i = idx; i + 1 < zcount; i++) zorder[i] = zorder[i + 1];
    zorder[zcount - 1] = w;
}

static uint32_t title_top_col(const struct gui_window *w) {
    return (w == focused) ? GUI_COL_TITLE_TOP : GUI_COL_TITLE_INA_TOP;
}
static uint32_t title_bot_col(const struct gui_window *w) {
    return (w == focused) ? GUI_COL_TITLE_BOT : GUI_COL_TITLE_INA_BOT;
}
static uint32_t title_col_at(const struct gui_window *w, int32_t y) {
    int t = y - (w->y + GUI_BORDER);
    if (t < 0) t = 0;
    if (t >= GUI_TITLE_H) t = GUI_TITLE_H - 1;
    return mix(title_top_col(w), title_bot_col(w), t, GUI_TITLE_H - 1);
}

#define BTN_SZ 18

static void close_rect(const struct gui_window *w,
                       int32_t *x, int32_t *y, uint32_t *s) {
    *s = BTN_SZ;
    *x = w->x + (int32_t)w->w - GUI_BORDER - 5 - (int32_t)*s;
    *y = w->y + GUI_BORDER + (GUI_TITLE_H - (int32_t)*s) / 2;
}

static void max_rect(const struct gui_window *w,
                     int32_t *x, int32_t *y, uint32_t *s) {
    close_rect(w, x, y, s);
    *x -= (int32_t)*s + 4;
}

static void draw_window(struct gui_window *w) {
    /* 拖动中的窗口走简化绘制：不画阴影、标题栏用纯色代替渐变。
       拖动由 IRQ12 触发、在主循环里合成，能省一点是一点。 */
    bool drag_simple = (dragging && w == drag_win);

    /* 阴影：两层，营造纵深（拖动时跳过） */
    if (!drag_simple) {
        gui_fill(w->x + 7, w->y + 7, w->w, w->h, 0x00060C14);
        gui_fill(w->x + 4, w->y + 4, w->w, w->h, 0x0009101A);
    }

    /* 外框：1px 深色 + 1px 亮色 */
    gui_fill(w->x, w->y, w->w, w->h, GUI_COL_BORDER_DARK);
    if (w->w > 2 && w->h > 2)
        gui_frame(w->x + 1, w->y + 1, w->w - 2, w->h - 2, GUI_COL_BORDER);

    bool act = (w == focused);
    int32_t tx = w->x + GUI_BORDER, ty = w->y + GUI_BORDER;
    uint32_t tw = w->w - 2 * GUI_BORDER;

    /* 标题栏：拖动时纯色，静止时渐变 */
    if (drag_simple)
        gui_fill(tx, ty, tw, GUI_TITLE_H, title_top_col(w));
    else
        gui_vgrad(tx, ty, tw, GUI_TITLE_H, title_top_col(w), title_bot_col(w));
    gui_fill(tx, ty, tw, 1, act ? 0x0069A6E8 : 0x003A5069);
    gui_fill(tx, ty + GUI_TITLE_H - 1, tw, 1, GUI_COL_BORDER_DARK);

    /* 标题（透明背景 + 轻微加粗） */
    int32_t tty = ty + (GUI_TITLE_H - FONT_HEIGHT) / 2;
    text_t(tx + 10, tty, w->title, (int)((tw - 78) / FONT_WIDTH), 0x00E8F2FF);
    text_t(tx + 11, tty, w->title, (int)((tw - 78) / FONT_WIDTH), 0x00E8F2FF);

    /* 最小化（最大化）按钮 */
    int32_t bx, by; uint32_t bs;
    max_rect(w, &bx, &by, &bs);
    bool hotmax = (hover_maxbtn && hover_win == w);
    gui_round_fill(bx, by, bs, bs,
                   hotmax ? 0x0040709F : 0x00315780, title_col_at(w, by));
    gfx_pixel(bx, by + bs - 1, title_col_at(w, by + (int32_t)bs - 1));
    gfx_pixel(bx + bs - 1, by + bs - 1, title_col_at(w, by + (int32_t)bs - 1));
    uint32_t mbtn = hotmax ? 0x0040709F : 0x00315780;
    if (w->maximized) {                    /* 还原图标：两个叠放的小方框 */
        gui_frame(bx + 4, by + 7, 7, 6, GUI_COL_WHITE);
        gui_fill(bx + 5, by + 8, 5, 4, mbtn);
        gui_frame(bx + 7, by + 4, 7, 6, GUI_COL_WHITE);
        gui_fill(bx + 8, by + 5, 5, 4, mbtn);
    } else {
        gui_frame(bx + 5, by + 5, 8, 8, GUI_COL_WHITE);
        gui_fill(bx + 6, by + 6, 6, 6, mbtn);
    }

    /* 关闭按钮 */
    close_rect(w, &bx, &by, &bs);
    bool hotcl = (hover_close && hover_win == w);
    uint32_t clbg = hotcl ? GUI_COL_CLOSE_HOVER
                          : (act ? GUI_COL_CLOSE : 0x00505F6E);
    gui_round_fill(bx, by, bs, bs, clbg, title_col_at(w, by));
    gfx_pixel(bx, by + bs - 1, title_col_at(w, by + (int32_t)bs - 1));
    gfx_pixel(bx + bs - 1, by + bs - 1, title_col_at(w, by + (int32_t)bs - 1));
    for (int32_t i = 0; i < 9; i++) {
        gui_fill(bx + 5 + i, by + 5 + i, 1, 1, GUI_COL_WHITE);
        gui_fill(bx + 12 - i, by + 5 + i, 1, 1, GUI_COL_WHITE);
    }

    /* 内容区：拖动中的窗口只留底色，松手时由 gui_mouse_event 补一帧完整内容 */
    gui_fill(gui_content_x(w), gui_content_y(w),
             gui_content_w(w), gui_content_h(w), GUI_COL_WINDOW_BG);
    if (w->draw && !drag_simple) w->draw(w);
}

struct gui_window *gui_window_open(const char *title, int32_t x, int32_t y,
                                   uint32_t w, uint32_t h) {
    if (zcount >= GUI_MAX_WINDOWS) return 0;

    struct gui_window *win = 0;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (!wins[i].used) { win = &wins[i]; break; }
    if (!win) return 0;

    memset(win, 0, sizeof(*win));
    win->used = true;
    win->w = w;
    win->h = h;
    copy_str(win->title, title, sizeof(win->title));

    int32_t maxx = (int32_t)gfx_width() - (int32_t)w;
    int32_t maxy = (int32_t)(gfx_height() - GUI_TASKBAR_H) - (int32_t)h;
    if (maxx < 0) maxx = 0;
    if (maxy < 0) maxy = 0;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > maxx) x = maxx;
    if (y > maxy) y = maxy;
    win->x = x; win->y = y;

    zorder[zcount++] = win;
    focused = win;
    task_add(win->title, TASK_KIND_APP, win);   /* 自动登记为应用任务 */
    gui_damage();
    return win;
}

void gui_window_close(struct gui_window *win) {
    if (!win || !win->used) return;
    if (win->on_close) win->on_close(win);
    task_remove_win(win);                        /* 注销对应应用任务 */

    for (int i = 0; i < zcount; i++) {
        if (zorder[i] != win) continue;
        for (int j = i; j + 1 < zcount; j++) zorder[j] = zorder[j + 1];
        zcount--;
        break;
    }
    win->used = false;
    if (drag_win == win) { dragging = 0; drag_win = 0; }
    if (focused == win) focused = zcount ? zorder[zcount - 1] : 0;
    if (hover_win == win) {
        hover_win = 0; hover_close = 0; hover_maxbtn = 0;
    }
    gui_damage();
}

void gui_window_focus(struct gui_window *win) {
    if (!win || !win->used) return;
    z_raise(win);
    focused = win;
    gui_damage();
}

bool gui_window_focused(const struct gui_window *win) { return focused == win; }

void gui_window_set_title(struct gui_window *win, const char *title) {
    if (!win || !win->used) return;
    copy_str(win->title, title, sizeof(win->title));
    task_rename_win(win, win->title);            /* 同步任务名 */
    gui_damage();
}

void gui_window_set_icon(struct gui_window *win, int icon) {
    if (!win || !win->used) return;
    win->icon = (uint8_t)icon;
    gui_damage();
}

bool gui_window_is_maximized(const struct gui_window *win) {
    return win && win->used && win->maximized;
}

void gui_window_maximize(struct gui_window *win) {
    if (!win || !win->used) return;
    if (!win->maximized) {
        win->rx = win->x; win->ry = win->y;
        win->rw = win->w; win->rh = win->h;
        win->x = 0; win->y = 0;
        win->w = gfx_width();
        win->h = gfx_height() - GUI_TASKBAR_H;
        win->maximized = true;
    } else {
        win->x = win->rx; win->y = win->ry;
        win->w = win->rw; win->h = win->rh;
        win->maximized = false;
    }
    if (win->on_resize) win->on_resize(win);
    gui_damage();
}

/* ---------------- 开始菜单 ---------------- */
#define ACT_NONE     0
#define ACT_PC       1
#define ACT_TERM     2
#define ACT_HOME     3
#define ACT_REBOOT   4
#define ACT_SHUTDOWN 5
#define ACT_DISK     6
#define ACT_TASKS    7
#define ACT_SYSMON   8

struct menu_item { const char *label; int action; int icon; };

static const struct menu_item menu_items[] = {
    { "此电脑",         ACT_PC,       GUI_ICON_PC },
    { "终端",           ACT_TERM,     GUI_ICON_TERM },
    { "个人文件夹",     ACT_HOME,     GUI_ICON_FOLDER },
    { "磁盘工具",       ACT_DISK,     GUI_ICON_DISK },
    { "任务管理器",     ACT_TASKS,    GUI_ICON_TASKS },
    { "系统监视器",     ACT_SYSMON,   GUI_ICON_MEM },
    { "",               ACT_NONE,     GUI_ICON_NONE },
    { "重启",           ACT_REBOOT,   GUI_ICON_RESTART },
    { "关机",           ACT_SHUTDOWN, GUI_ICON_POWER },
};
#define MENU_N ((int)(sizeof(menu_items) / sizeof(menu_items[0])))

static int32_t start_x(void) { return START_PAD; }
static int32_t start_y(void) {
    return (int32_t)gfx_height() - GUI_TASKBAR_H + (GUI_TASKBAR_H - START_H) / 2;
}

static uint32_t menu_h(void) {
    int32_t h = MENU_HDR_H + MENU_PAD * 2;
    for (int i = 0; i < MENU_N; i++)
        h += (menu_items[i].action == ACT_NONE) ? MENU_SEP_H : MENU_ITEM_H;
    return (uint32_t)h;
}

static int32_t menu_x(void) { return START_PAD; }
static int32_t menu_y(void) {
    return (int32_t)gfx_height() - GUI_TASKBAR_H - 5 - (int32_t)menu_h();
}

static void menu_item_rect(int i, int32_t *x, int32_t *y,
                           int32_t *w, int32_t *h) {
    int32_t yy = menu_y() + MENU_HDR_H + MENU_PAD;
    for (int k = 0; k < i; k++)
        yy += (menu_items[k].action == ACT_NONE) ? MENU_SEP_H : MENU_ITEM_H;
    *x = menu_x() + 4;
    *y = yy;
    *w = MENU_W - 8;
    *h = (menu_items[i].action == ACT_NONE) ? MENU_SEP_H : MENU_ITEM_H;
}

static int menu_hit(int32_t px, int32_t py) {
    if (!start_open) return -1;
    if (!gui_hit(px, py, menu_x(), menu_y(), MENU_W, menu_h())) return -1;
    for (int i = 0; i < MENU_N; i++) {
        int32_t x, y, w, h;
        menu_item_rect(i, &x, &y, &w, &h);
        if (gui_hit(px, py, x, y, (uint32_t)w, (uint32_t)h)) return i;
    }
    return -1;
}

static void draw_start_menu(void) {
    int32_t x = menu_x(), y = menu_y();
    uint32_t w = MENU_W, h = menu_h();

    /* 阴影 */
    gui_fill(x + 6, y + 6, w, h, 0x00060C14);
    gui_fill(x + 3, y + 3, w, h, 0x0009101A);

    gui_fill(x, y, w, h, GUI_COL_MENU);
    gui_frame(x, y, w, h, GUI_COL_BORDER_DARK);
    gui_fill(x + 1, y + 1, w - 2, 1, GUI_COL_BORDER);

    /* 头部 */
    gui_fill(x + 1, y + 1, w - 2, MENU_HDR_H, GUI_COL_MENU_HDR);
    fill_circle(x + 23, y + 22, 11, GUI_COL_ACCENT);
    gui_text(x + 19, y + 14, "G", GUI_COL_WHITE, GUI_COL_ACCENT);
    gui_text_bold(x + 43, y + 8, "StarRing OS", GUI_COL_WHITE, GUI_COL_MENU_HDR);
    gui_text(x + 43, y + 24, "guest", GUI_COL_TEXT_DIM, GUI_COL_MENU_HDR);
    gui_fill(x + 1, y + MENU_HDR_H, w - 2, 1, GUI_COL_BORDER);

    for (int i = 0; i < MENU_N; i++) {
        int32_t ix, iy, iw, ih;
        menu_item_rect(i, &ix, &iy, &iw, &ih);
        if (menu_items[i].action == ACT_NONE) {
            gui_fill(ix + 10, iy + ih / 2, (uint32_t)(iw - 20), 1,
                     GUI_COL_BORDER);
            continue;
        }
        bool hot = (menu_hover == i);
        uint32_t bg = hot ? GUI_COL_SELECT : GUI_COL_MENU;
        uint32_t fg = GUI_COL_TEXT;
        if (menu_items[i].action == ACT_SHUTDOWN) fg = 0x00FFB3A6;
        gui_fill(ix, iy, (uint32_t)iw, (uint32_t)ih, bg);
        if (hot) gui_fill(ix, iy, 3, (uint32_t)ih, GUI_COL_ACCENT);
        gui_draw_mini_icon(ix + 14, iy + (ih - 16) / 2, menu_items[i].icon);
        gui_text(ix + 40, iy + (ih - FONT_HEIGHT) / 2,
                 menu_items[i].label, fg, bg);
    }
}

static void menu_activate(int i) {
    int act = menu_items[i].action;
    start_open = 0;
    menu_hover = -1;

    switch (act) {
    case ACT_PC:       files_open("/"); break;
    case ACT_TERM:     terminal_open(); break;
    case ACT_HOME:     files_open("/home/guest"); break;
    case ACT_DISK:     disk_utility_open(); break;
    case ACT_TASKS:    taskmgr_open(); break;
    case ACT_SYSMON:   sysmon_open(); break;
    case ACT_REBOOT:   power_reboot(); break;
    case ACT_SHUTDOWN: power_shutdown(); break;
    default: break;
    }
    gui_damage();
}

/* ---------------- 右键上下文菜单 ----------------
   单例菜单，画在所有窗口/任务栏之上。样式与开始菜单一致（MENU 配色）。
   项高 CTX_ITEM_H，分隔线 CTX_SEP_H，四周留 CTX_PAD 内边距。 */
#define CTX_ITEM_H  26
#define CTX_SEP_H   7
#define CTX_PAD     4

static int         ctx_open = 0;
static int32_t     ctx_x = 0, ctx_y = 0;
static uint32_t    ctx_w = 0, ctx_h = 0;
static const char *const *ctx_labels = 0;
static const uint8_t *ctx_flags = 0;
static int         ctx_n = 0;
static int16_t     ctx_ih[GUI_CTX_MAX_ITEMS];
static int         ctx_hover = -1;
static void      (*ctx_pick)(int index, void *user) = 0;
static void       *ctx_user = 0;

static bool ctx_enabled(int i) {
    return !ctx_flags || !(ctx_flags[i] & GUI_CTX_DISABLED);
}

static void ctx_item_rect(int i, int32_t *x, int32_t *y,
                          uint32_t *w, uint32_t *h) {
    int32_t yy = ctx_y + CTX_PAD;
    for (int k = 0; k < i; k++) yy += ctx_ih[k];
    *x = ctx_x + 2;
    *y = yy;
    *w = ctx_w - 4;
    *h = (uint32_t)ctx_ih[i];
}

static int ctx_hit(int32_t px, int32_t py) {
    if (!ctx_open || !ctx_labels) return -1;
    if (!gui_hit(px, py, ctx_x, ctx_y, ctx_w, ctx_h)) return -1;
    for (int i = 0; i < ctx_n; i++) {
        if (!ctx_labels[i]) continue;
        int32_t x, y; uint32_t w, h;
        ctx_item_rect(i, &x, &y, &w, &h);
        if (gui_hit(px, py, x, y, w, h)) return i;
    }
    return -1;
}

static void draw_context_menu(void) {
    if (!ctx_open || !ctx_labels) return;
    int32_t  x = ctx_x, y = ctx_y;
    uint32_t w = ctx_w, h = ctx_h;

    gui_fill(x + 5, y + 5, w, h, 0x00060C14);
    gui_fill(x + 2, y + 2, w, h, 0x0009101A);
    gui_fill(x, y, w, h, GUI_COL_MENU);
    gui_frame(x, y, w, h, GUI_COL_BORDER_DARK);
    gui_fill(x + 1, y + 1, w - 2, 1, GUI_COL_BORDER);

    for (int i = 0; i < ctx_n; i++) {
        int32_t ix, iy; uint32_t iw, ih;
        ctx_item_rect(i, &ix, &iy, &iw, &ih);
        if (!ctx_labels[i]) {
            gui_fill(ix + 6, iy + (int32_t)ih / 2, iw - 12, 1, GUI_COL_BORDER);
            continue;
        }
        bool enabled = ctx_enabled(i);
        bool hot = enabled && (ctx_hover == i);
        uint32_t bg = hot ? GUI_COL_SELECT : GUI_COL_MENU;
        gui_fill(ix, iy, iw, ih, bg);
        if (hot) gui_fill(ix, iy, 3, ih, GUI_COL_ACCENT);
        gui_text(ix + 14, iy + ((int32_t)ih - FONT_HEIGHT) / 2,
                 ctx_labels[i], enabled ? GUI_COL_TEXT : 0x00506A82, bg);
    }
}

void gui_context_open(int32_t x, int32_t y, const char *const *labels, int count,
                      void (*on_pick)(int index, void *user), void *user) {
    gui_context_open_ex(x, y, labels, 0, count, on_pick, user);
}

void gui_context_open_ex(int32_t x, int32_t y, const char *const *labels,
                         const uint8_t *flags, int count,
                         void (*on_pick)(int index, void *user), void *user) {
    if (!labels || count <= 0 || count > GUI_CTX_MAX_ITEMS) return;

    uint32_t w = 0;
    for (int i = 0; i < count; i++) {
        ctx_ih[i] = (int16_t)(labels[i] ? CTX_ITEM_H : CTX_SEP_H);
        if (labels[i]) {
            uint32_t tw = gui_str_width(labels[i]);
            if (tw > w) w = tw;
        }
    }
    w += 32;
    if (w < 130) w = 130;

    uint32_t h = CTX_PAD * 2;
    for (int i = 0; i < count; i++) h += (uint32_t)ctx_ih[i];

    /* 夹到屏幕内，避免菜单跑出边界 */
    int32_t sw = (int32_t)gfx_width(), sh = (int32_t)gfx_height();
    if (x + (int32_t)w > sw - 2) x = sw - 2 - (int32_t)w;
    if (y + (int32_t)h > sh - 2) y = sh - 2 - (int32_t)h;
    if (x < 2) x = 2;
    if (y < 2) y = 2;

    ctx_x = x; ctx_y = y; ctx_w = w; ctx_h = h;
    ctx_labels = labels; ctx_flags = flags; ctx_n = count;
    ctx_pick = on_pick; ctx_user = user;
    ctx_hover = -1;
    ctx_open = 1;
    gui_damage();
}

void gui_context_close(void) {
    if (!ctx_open) return;
    ctx_open = 0;
    ctx_labels = 0; ctx_flags = 0; ctx_n = 0;
    ctx_pick = 0; ctx_user = 0;
    ctx_hover = -1;
    gui_damage();
}

bool gui_context_active(void) { return ctx_open != 0; }

static void ctx_activate(int i) {
    if (i < 0 || i >= ctx_n || !ctx_labels || !ctx_labels[i]) return;
    if (!ctx_enabled(i)) return;         /* 灰显项：保持菜单打开 */
    void (*pick)(int, void *) = ctx_pick;
    void *user = ctx_user;
    gui_context_close();
    if (pick) pick(i, user);
}

/* ---------------- 桌面右键菜单 ---------------- */

static void desk_ctx_pick(int index, void *user) {
    (void)user;
    switch (index) {
    case 0: gui_damage();      break;   /* Refresh */
    case 2: terminal_open();   break;   /* Open Terminal */
    case 3: files_open("/");    break;  /* This PC */
    default: break;
    }
}

/* NULL = 分隔线，下标与 desk_ctx_pick 的分支严格对应 */
static const char *const desk_ctx_items[] = {
    "刷新", 0, "打开终端", "此电脑",
};
#define DESK_CTX_N ((int)(sizeof(desk_ctx_items) / sizeof(desk_ctx_items[0])))

static void desk_rclick(void) {
    gui_context_open(mx, my, desk_ctx_items, DESK_CTX_N, desk_ctx_pick, 0);
}

/* ---------------- 窗口右键菜单 ----------------
   应用没有提供 on_rclick 时的通用回退：最大化/还原 + 关闭。 */
static struct gui_window *ctx_win = 0;

static const char *const win_ctx_items[]     = { "最大化", 0, "关闭" };
static const char *const win_ctx_items_max[] = { "还原",   0, "关闭" };
#define WIN_CTX_N ((int)(sizeof(win_ctx_items) / sizeof(win_ctx_items[0])))

static void win_ctx_pick(int index, void *user) {
    (void)user;
    struct gui_window *w = ctx_win;
    ctx_win = 0;
    if (!w || !w->used) return;
    if (index == 0)      gui_window_maximize(w);
    else if (index == 2) gui_window_close(w);
}

static void win_rclick(struct gui_window *w) {
    ctx_win = w;
    gui_context_open(mx, my, w->maximized ? win_ctx_items_max : win_ctx_items,
                     WIN_CTX_N, win_ctx_pick, 0);
}

/* ---------------- 桌面图标 ---------------- */
static void icon_open_pc(void)   { files_open("/"); }
static void icon_open_term(void) { terminal_open(); }
static void icon_open_disk(void) { disk_utility_open(); }
static void icon_open_tasks(void){ taskmgr_open(); }
static void icon_open_sysmon(void){ sysmon_open(); }

struct desk_icon {
    const char *label;
    int         icon;
    void      (*open)(void);
};

static const struct desk_icon desk_icons[] = {
    { "此电脑",         GUI_ICON_PC,    icon_open_pc },
    { "终端",           GUI_ICON_TERM,  icon_open_term },
    { "磁盘工具",       GUI_ICON_DISK,  icon_open_disk },
    { "任务管理器",     GUI_ICON_TASKS, icon_open_tasks },
    { "系统监视器",     GUI_ICON_MEM,   icon_open_sysmon },
};
#define DESK_N ((int)(sizeof(desk_icons) / sizeof(desk_icons[0])))

/* 桌面图标右键菜单：打开 / 属性（属性目前只对“此电脑”有效） */
static int ctx_icon_idx = -1;
static const char *const icon_ctx_items[] = { "打开", 0, "属性" };
#define ICON_CTX_N ((int)(sizeof(icon_ctx_items) / sizeof(icon_ctx_items[0])))

static void desk_ctx_icon_pick(int index, void *user) {
    (void)user;
    if (ctx_icon_idx < 0 || ctx_icon_idx >= DESK_N) return;
    if (index == 0) {
        desk_icons[ctx_icon_idx].open();
    } else if (index == 2 && ctx_icon_idx == 0) {
        sysprop_open();              /* 索引 0 = “此电脑” */
    }
}

#define ICON_BX(x) ((x) - 10)
#define ICON_BY(y) ((y) - 10)
#define ICON_BW    (ICON_W + 20)
#define ICON_BH    (ICON_H + 34)

static int32_t icon_y(int i) { return ICON_Y0 + i * ICON_DY; }

static int icon_hit(int32_t px, int32_t py) {
    for (int i = 0; i < DESK_N; i++) {
        if (gui_hit(px, py, ICON_BX(ICON_X), ICON_BY(icon_y(i)),
                    ICON_BW, ICON_BH)) return i;
    }
    return -1;
}

static void draw_desktop_icons(void) {
    for (int i = 0; i < DESK_N; i++) {
        int32_t x = ICON_X, y = icon_y(i);
        /* 屏幕外的图标整块跳过（含图标位图与中文标签） */
        if (!rect_hits_clip(ICON_BX(x), ICON_BY(y), ICON_BW, ICON_BH)) continue;
        bool sel = (desk_sel == i);
        bool hot = (hover_icon == i);
        int32_t bxx = ICON_BX(x), byy = ICON_BY(y);

        if (sel || hot) {
            uint32_t bg = sel ? 0x003A6699 : 0x002A4763;
            gui_round_fill(bxx, byy, ICON_BW, ICON_BH, bg,
                           wall_at((uint32_t)byy));
            gfx_pixel(bxx, byy + (int32_t)ICON_BH - 1,
                      wall_at((uint32_t)(byy + ICON_BH - 1)));
            gfx_pixel(bxx + (int32_t)ICON_BW - 1, byy + (int32_t)ICON_BH - 1,
                      wall_at((uint32_t)(byy + ICON_BH - 1)));
            if (sel) gui_frame(bxx, byy, ICON_BW, ICON_BH, GUI_COL_ACCENT);
        }

        switch (desk_icons[i].icon) {
        case GUI_ICON_PC:    gui_icon_pc(x, y);    break;
        case GUI_ICON_DISK:  gui_icon_disk(x, y);  break;
        case GUI_ICON_TASKS: gui_icon_tasks(x, y); break;
        case GUI_ICON_MEM:   gui_icon_mem(x, y);   break;
        default:             gui_icon_term(x, y);  break;
        }

        uint32_t tw = gui_str_width(desk_icons[i].label);
        int32_t lx = x + ((int32_t)ICON_W - (int32_t)tw) / 2;
        int32_t ly = y + ICON_H + 2;
        if (sel || hot) {
            uint32_t lbg = sel ? 0x003A6699 : 0x002A4763;
            gui_fill(bxx + 2, ly - 2, ICON_BW - 4, FONT_HEIGHT + 3, lbg);
            gui_text(lx, ly, desk_icons[i].label, GUI_COL_WHITE, lbg);
        } else {
            /* 中文标签必须走 TTF：8x16 点阵没有 CJK 字形 */
            gui_text(lx, ly, desk_icons[i].label, GUI_COL_WHITE, 0);
        }
    }
}

/* ---------------- 任务栏 ---------------- */
static int taskbar_count(void) {
    int n = 0;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) if (wins[i].used) n++;
    return n;
}

static struct gui_window *taskbar_win(int idx) {
    int k = 0;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!wins[i].used) continue;
        if (k == idx) return &wins[i];
        k++;
    }
    return 0;
}

static uint32_t task_btn_w(const struct gui_window *w) {
    uint32_t bw = 40 + gui_str_width(w->title);
    if (bw < 112) bw = 112;
    if (bw > 210) bw = 210;
    return bw;
}

static int32_t tb_x0(void) { return start_x() + START_W + 10; }
static int32_t tb_limit(void) { return (int32_t)gfx_width() - 160; }

static int32_t tb_btn_x(int idx) {
    int32_t x = tb_x0();
    for (int i = 0; i < idx; i++) {
        struct gui_window *w = taskbar_win(i);
        if (w) x += (int32_t)task_btn_w(w) + 6;
    }
    return x;
}

static int taskbar_hit_index(int32_t px, int32_t py) {
    if (py < start_y() || py >= start_y() + START_H) return -1;
    int n = taskbar_count();
    for (int i = 0; i < n; i++) {
        struct gui_window *w = taskbar_win(i);
        if (!w) continue;
        int32_t x = tb_btn_x(i);
        uint32_t bw = task_btn_w(w);
        if (x + (int32_t)bw > tb_limit()) break;
        if (gui_hit(px, py, x, start_y(), bw, START_H)) return i;
    }
    return -1;
}

static void two_digits(char *out, int v) {
    if (v < 0) v = 0;
    if (v > 99) v = 99;
    out[0] = (char)('0' + (v / 10) % 10);
    out[1] = (char)('0' + v % 10);
}

static void clock_text(char *out) {
    int h, m, s;
    uint64_t up = timer_seconds();
    if (clk_h >= 0) {
        uint64_t t = ((uint64_t)clk_h * 3600 + (uint64_t)clk_m * 60 +
                      (uint64_t)clk_s + up) % 86400u;
        h = (int)(t / 3600); m = (int)((t / 60) % 60); s = (int)(t % 60);
    } else {
        h = (int)(up / 3600); m = (int)((up / 60) % 60); s = (int)(up % 60);
    }
    two_digits(out, h);     out[2] = ':';
    two_digits(out + 3, m); out[5] = ':';
    two_digits(out + 6, s); out[8] = 0;
}

static void draw_taskbar(void) {
    uint32_t sw = gfx_width(), sh = gfx_height();
    int32_t ty = (int32_t)sh - GUI_TASKBAR_H;
    int32_t sy = start_y();

    gui_vgrad(0, ty, sw, GUI_TASKBAR_H,
              GUI_COL_TASKBAR_TOP, GUI_COL_TASKBAR_BOT);
    gui_fill(0, ty, sw, 1, GUI_COL_TASKBAR_LINE);

    /* 开始按钮 */
    bool hot = start_open || start_hover;
    uint32_t sbg = start_open ? GUI_COL_ACCENT
                              : (start_hover ? GUI_COL_BTN_HOVER : GUI_COL_BTN);
    gui_round_fill(start_x(), sy, START_W, START_H, sbg,
                   taskbar_col(sy));
    gui_frame(start_x(), sy, START_W, START_H,
              hot ? GUI_COL_ACCENT : 0x003A5C80);
    gfx_pixel(start_x(), sy, taskbar_col(sy));
    gfx_pixel(start_x() + START_W - 1, sy, taskbar_col(sy));
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++)
            gui_fill(start_x() + 11 + i * 8, sy + 7 + j * 7, 6, 6,
                     start_open ? GUI_COL_WHITE : GUI_COL_ACCENT);
    gui_text(start_x() + 34, sy + (START_H - FONT_HEIGHT) / 2, "开始",
             GUI_COL_WHITE, sbg);

    /* 分隔线 */
    gui_fill(tb_x0() - 6, ty + 9, 1, GUI_TASKBAR_H - 18, 0x00304C68);

    /* 窗口按钮 */
    int n = taskbar_count();
    for (int i = 0; i < n; i++) {
        struct gui_window *w = taskbar_win(i);
        if (!w) continue;
        int32_t x = tb_btn_x(i);
        uint32_t bw = task_btn_w(w);
        if (x + (int32_t)bw > tb_limit()) break;
        bool act = (w == focused);
        bool h = (hover_task == i);
        uint32_t bg = act ? GUI_COL_BTN_HOVER
                          : (h ? GUI_COL_PANEL_LIGHT : GUI_COL_BTN);
        gui_round_fill(x, sy, bw, START_H, bg, taskbar_col(sy));
        gui_frame(x, sy, bw, START_H, act ? GUI_COL_ACCENT : 0x003A5C80);
        gfx_pixel(x, sy, taskbar_col(sy));
        gfx_pixel(x + (int32_t)bw - 1, sy, taskbar_col(sy));
        if (w->icon) gui_draw_mini_icon(x + 8, sy + 5, w->icon);
        text_t(x + 30, sy + (START_H - FONT_HEIGHT) / 2, w->title,
               (int)((bw - 38) / FONT_WIDTH),
               act ? GUI_COL_WHITE : GUI_COL_TEXT);
        if (act) gui_fill(x + 2, sy + START_H - 3, bw - 4, 2, GUI_COL_ACCENT);
    }

    /* 时钟（日期 + 时间） */
    char tm[16], dt[16];
    clock_text(tm);
    dt[0] = 0;
    if (clk_day) {
        char b[8];
        kstrcpy(dt, utoa((uint64_t)clk_year, b, 10));
        dt[4] = '-';
        two_digits(dt + 5, clk_mon); dt[7] = '-';
        two_digits(dt + 8, clk_day); dt[10] = 0;
    }
    int32_t tty = ty + (GUI_TASKBAR_H - FONT_HEIGHT) / 2;
    /* 宽度用 TTF 量、字形也必须用 TTF 画，否则右对齐会错位 */
    uint32_t tw = gui_str_width(tm);
    int32_t tx = (int32_t)sw - (int32_t)tw - 14;
    gui_text(tx, tty, tm, GUI_COL_WHITE, 0);
    if (dt[0]) {
        uint32_t dw = gui_str_width(dt);
        gui_text(tx - (int32_t)dw - 14, tty, dt, GUI_COL_TEXT_DIM, 0);
        gui_fill(tx - 8, ty + 9, 1, GUI_TASKBAR_H - 18, 0x00304C68);
    }
}

static void taskbar_click(int32_t px, int32_t py) {
    if (gui_hit(px, py, start_x(), start_y(), START_W, START_H)) {
        start_open = !start_open;
        menu_hover = -1;
        dmg_menu();
        return;
    }
    int idx = taskbar_hit_index(px, py);
    if (idx >= 0) {
        struct gui_window *w = taskbar_win(idx);
        if (w) {
            if (w == focused) gui_window_maximize(w);   /* 已聚焦：最大化/还原 */
            else              gui_window_focus(w);
        }
    }
}

/* ---------------- 合成 ---------------- */
static void desktop_draw(void) {
    uint32_t sw = gfx_width(), sh = gfx_height();
    int32_t ty = (int32_t)sh - GUI_TASKBAR_H;

    /* 壁纸：直接用开机遮罩预计算好的每行颜色，一次调用铺满整屏
       （内层直接写 backbuffer，没有逐行 mix()/gfx_rect 调用）。
       缓存还没准备好（比如遮罩被跳过）时退回逐行实时计算。 */
    if (bg_cache_valid())
        gfx_fill_row_colors(0, 0, sw, sh, bg_gradient_cache, bg_cache_rows);
    else
        for (uint32_t y = 0; y < sh; y++)
            gfx_rect(0, (int32_t)y, sw, 1, wall_color(y));

    if (rect_hits_clip(0, ty - 1, sw, 1))
        gui_fill(0, ty - 1, sw, 1, 0x00203A56);   /* 任务栏上方细线 */

    draw_desktop_icons();
    for (int i = 0; i < zcount; i++) {
        struct gui_window *w = zorder[i];
        /* 阴影外扩 7px，包围盒留足余量；完全在裁剪区外的窗口整窗跳过 */
        if (!rect_hits_clip(w->x - 2, w->y - 2, w->w + 10, w->h + 10)) continue;
        draw_window(w);
    }
    if (rect_hits_clip(0, ty, sw, GUI_TASKBAR_H)) draw_taskbar();
    /* 菜单阴影向外扩 6px，包围盒一起放宽，避免裁掉阴影边缘 */
    if (start_open && rect_hits_clip(menu_x(), menu_y(), MENU_W + 8, menu_h() + 8))
        draw_start_menu();

    /* 右下角水印 */
    {
        const char *wm = "StarRing OS 0.1";
        uint32_t ww = gui_str_width(wm);
        int32_t wx = (int32_t)sw - (int32_t)ww - 18;
        int32_t wy = ty - 26;
        if (rect_hits_clip(wx, wy, ww, FONT_HEIGHT))
            gui_text(wx, wy, wm,
                     mix(wall_at((uint32_t)wy), GUI_COL_ACCENT, 55, 100), 0);
    }

    /* 右键菜单画在最上层（窗口/任务栏/开始菜单之上） */
    if (ctx_open && rect_hits_clip(ctx_x, ctx_y, ctx_w + 6, ctx_h + 6))
        draw_context_menu();
}

void gui_damage(void) { need_composite = 1; }

uint64_t gui_idle_ticks(void) { return idle_ticks; }

void gui_set_slow_tick(void (*fn)(uint64_t ticks)) { slow_tick_fn = fn; }

void gui_set_login_hooks(
        void (*draw)(void),
        void (*on_mouse)(uint8_t buttons, int32_t dx, int32_t dy),
        void (*on_key)(uint16_t key),
        void (*on_tick)(uint64_t ticks)) {
    login_draw_fn  = draw;
    login_mouse_fn = on_mouse;
    login_key_fn   = on_key;
    login_tick_fn  = on_tick;
}

void gui_clear_login_hooks(void) {
    login_draw_fn  = 0;
    login_mouse_fn = 0;
    login_key_fn   = 0;
    login_tick_fn  = 0;
}

/* 是否有待处理的损坏区（整屏或局部） */
static inline int damage_pending(void) { return need_composite || dmg_able; }

void gui_damage_rect(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return;
    if (need_composite) return;             /* 已整屏重绘，无需再并集 */
    int32_t x1 = x + (int32_t)w, y1 = y + (int32_t)h;
    if (!dmg_able) {
        dmg_x0 = x; dmg_y0 = y; dmg_x1 = x1; dmg_y1 = y1;
        dmg_able = 1;
        return;
    }
    if (x  < dmg_x0) dmg_x0 = x;
    if (y  < dmg_y0) dmg_y0 = y;
    if (x1 > dmg_x1) dmg_x1 = x1;
    if (y1 > dmg_y1) dmg_y1 = y1;
}

static void composite(void) {
    if (in_composite) { need_composite = 1; return; }

    for (;;) {
        int32_t  rx, ry;
        uint32_t rw, rh;
        uint64_t fl = irq_save();
        int full = (need_composite || !dmg_able);
        if (full) {
            rx = 0; ry = 0; rw = gfx_width(); rh = gfx_height();
        } else {
            rx = dmg_x0; ry = dmg_y0;
            rw = (uint32_t)(dmg_x1 - dmg_x0);
            rh = (uint32_t)(dmg_y1 - dmg_y0);
        }
        need_composite = 0;
        dmg_able = 0;
        irq_restore(fl);

        /* 夹到屏幕内；完全在屏幕外时无需绘制 */
        int32_t x0 = rx, y0 = ry;
        int32_t x1 = rx + (int32_t)rw, y1 = ry + (int32_t)rh;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int32_t)gfx_width())  x1 = (int32_t)gfx_width();
        if (y1 > (int32_t)gfx_height()) y1 = (int32_t)gfx_height();

        if (x0 < x1 && y0 < y1) {
            uint32_t cw = (uint32_t)(x1 - x0), chh = (uint32_t)(y1 - y0);
            in_composite = 1;
            gfx_clip_set(x0, y0, cw, chh);
            comp_x0 = x0; comp_y0 = y0;
            comp_w = cw;  comp_h = chh;
            comp_valid = 1;
            pointer_hide();
            if (login_draw_fn) login_draw_fn();
            else               desktop_draw();
            comp_valid = 0;
            gfx_clip_reset();
            gfx_present_rect(x0, y0, cw, chh);   /* 末尾由 hook 叠加指针 */
            in_composite = 0;
        }

        if (!need_composite && !dmg_able) break;
    }
}

/* ---------------- 长任务队列 ----------------
   鼠标的 on_click / on_hover 是在 IRQ1 中断上下文里被调用的（见 mouse.c），
   那里既不适合跑「格式化磁盘」这种长任务，也不适合做合成上屏。
   应用用 gui_post_action() 把工作投递给 gui_run() 的循环体去执行；
   被投递的函数运行在开中断的主循环上下文，可以用 gui_repaint()
   同步刷出进度动画的每一帧。 */
#define ACTION_Q 8
static void (*action_q[ACTION_Q])(void);
static volatile int action_head, action_tail;

bool gui_post_action(void (*fn)(void)) {
    if (!fn) return false;
    uint64_t fl = irq_save();
    int  head   = action_head;
    int  next   = (head + 1) % ACTION_Q;
    bool ok     = (next != action_tail);      /* 队列满则丢弃，由调用方回退 */
    if (ok) {
        action_q[head] = fn;
        action_head    = next;
    }
    irq_restore(fl);
    return ok;
}

/* 立即合成并上屏一帧。只能在主循环上下文调用（即 action 函数内部）；
   在中断里调用会和正在进行的合成重入，composite() 自己有 in_composite
   保护，那种情况下只会退化成一次 gui_damage()，不会画花屏幕。 */
void gui_repaint(void) { composite(); }

static void run_actions(void) {
    for (;;) {
        void (*fn)(void);
        uint64_t fl = irq_save();
        int t       = action_tail;
        fn          = (t != action_head) ? action_q[t] : 0;
        if (fn) action_tail = (t + 1) % ACTION_Q;
        irq_restore(fl);
        if (!fn) break;
        fn();                       /* 主循环上下文，中断已开启 */
    }
}

/* ---------------- 悬停 ---------------- */
static struct gui_window *win_at(int32_t px, int32_t py) {
    for (int i = zcount - 1; i >= 0; i--)
        if (gui_hit(px, py, zorder[i]->x, zorder[i]->y,
                    zorder[i]->w, zorder[i]->h))
            return zorder[i];
    return 0;
}

/* ---- 局部脏区辅助 ---- */
static void dmg_title(const struct gui_window *w) {
    if (!w) return;
    gui_damage_rect(w->x, w->y, w->w,
                    (uint32_t)(GUI_BORDER + GUI_TITLE_H + 2));
}

static void dmg_window(const struct gui_window *w) {
    if (!w) return;
    gui_damage_rect(w->x - 1, w->y - 1, w->w + 9, w->h + 9);   /* 含阴影 */
}

static void dmg_task_btn(int idx) {
    if (idx < 0) return;
    struct gui_window *w = taskbar_win(idx);
    if (!w) return;
    int32_t x = tb_btn_x(idx);
    uint32_t bw = task_btn_w(w);
    if (x + (int32_t)bw > tb_limit()) return;
    gui_damage_rect(x - 1, start_y() - 1, bw + 2, START_H + 2);
}

static void dmg_icon(int idx) {
    if (idx < 0 || idx >= DESK_N) return;
    gui_damage_rect(ICON_BX(ICON_X) - 1, ICON_BY(icon_y(idx)) - 1,
                    ICON_BW + 2, ICON_BH + 2);
}

static void dmg_start_btn(void) {
    gui_damage_rect(start_x() - 2, start_y() - 2, START_W + 4, START_H + 4);
}

static void dmg_menu(void) {
    gui_damage_rect(menu_x(), menu_y() - 1, MENU_W + 8, menu_h() + 8);
    dmg_start_btn();
}

static void update_hover(void) {
    /* 右键菜单打开时：只更新菜单自身的高亮，冻结下层元素 */
    if (ctx_open) {
        int ch = ctx_hit(mx, my);
        if (ch >= 0 && !ctx_enabled(ch)) ch = -1;
        if (ch != ctx_hover) {
            int32_t x, y; uint32_t w, h;
            if (ctx_hover >= 0) {
                ctx_item_rect(ctx_hover, &x, &y, &w, &h);
                gui_damage_rect(x, y, w, h);
            }
            if (ch >= 0) {
                ctx_item_rect(ch, &x, &y, &w, &h);
                gui_damage_rect(x, y, w, h);
            }
            ctx_hover = ch;
        }
        return;
    }

    int32_t sh = (int32_t)gfx_height();
    struct gui_window *w = win_at(mx, my);
    int task = -1, icon = -1, mh = -1, cl = 0, mb = 0;
    int st = gui_hit(mx, my, start_x(), start_y(), START_W, START_H);

    if (w) {
        int32_t bx, by; uint32_t bs;
        close_rect(w, &bx, &by, &bs);
        cl = gui_hit(mx, my, bx, by, bs, bs);
        max_rect(w, &bx, &by, &bs);
        mb = gui_hit(mx, my, bx, by, bs, bs);
    }
    if (!w) {
        if (my >= sh - GUI_TASKBAR_H) task = taskbar_hit_index(mx, my);
        else if (!start_open)         icon = icon_hit(mx, my);
    }
    if (start_open) {
        mh = menu_hit(mx, my);
        if (mh >= 0 && menu_items[mh].action == ACT_NONE) mh = -1;
    }

    /* ---- 只重绘真正变化的元素 ---- */
    if (st != start_hover) dmg_start_btn();

    if (mh != menu_hover) {
        int32_t x, y, ww, hh;
        if (menu_hover >= 0) {
            menu_item_rect(menu_hover, &x, &y, &ww, &hh);
            gui_damage_rect(x, y, (uint32_t)ww, (uint32_t)hh);
        }
        if (mh >= 0) {
            menu_item_rect(mh, &x, &y, &ww, &hh);
            gui_damage_rect(x, y, (uint32_t)ww, (uint32_t)hh);
        }
    }

    if (task != hover_task) { dmg_task_btn(hover_task); dmg_task_btn(task); }
    if (icon != hover_icon) { dmg_icon(hover_icon); dmg_icon(icon); }

    if (w != hover_win) {
        if (hover_win) dmg_title(hover_win);
        if (w)         dmg_title(w);
    } else if (w && (cl != hover_close || mb != hover_maxbtn)) {
        dmg_title(w);
    }

    /* ---- 通知应用 ---- */
    if (w != hover_win) {
        if (hover_win && hover_win->used && hover_win->on_hover)
            hover_win->on_hover(hover_win, -1, -1);
        hover_win = w;
    }
    if (hover_win && hover_win->on_hover)
        hover_win->on_hover(hover_win,
                            mx - gui_content_x(hover_win),
                            my - gui_content_y(hover_win));

    hover_close = cl; hover_maxbtn = mb; hover_task = task;
    hover_icon = icon; start_hover = st ? 1 : 0; menu_hover = mh;
}

/* ---------------- 鼠标事件 ---------------- */
static void mouse_press(void) {
    int32_t sh = (int32_t)gfx_height();
    int32_t ty = sh - GUI_TASKBAR_H;
    app_drag_win = 0;

    /* 右键菜单打开：左键落在菜单上就选中，落在别处则关掉菜单再按普通点击处理 */
    if (ctx_open) {
        int ci = ctx_hit(mx, my);
        if (ci >= 0) { ctx_activate(ci); return; }
        gui_context_close();
    }

    if (start_open) {
        int mi = menu_hit(mx, my);
        if (mi >= 0) {
            if (menu_items[mi].action != ACT_NONE) menu_activate(mi);
            return;
        }
        int on_start = gui_hit(mx, my, start_x(), start_y(), START_W, START_H);
        start_open = 0;
        menu_hover = -1;
        dmg_menu();
        if (on_start) return;      /* 点在开始按钮上：只关闭菜单 */
        /* 其他位置：关闭菜单后继续按普通点击处理 */
    }

    if (gui_hit(mx, my, start_x(), start_y(), START_W, START_H)) {
        start_open = 1;
        menu_hover = -1;
        dmg_menu();
        return;
    }

    if (my >= ty) {
        taskbar_click(mx, my);
        return;
    }

    /* 窗口：从最上层往下找 */
    for (int i = zcount - 1; i >= 0; i--) {
        struct gui_window *w = zorder[i];
        if (!gui_hit(mx, my, w->x, w->y, w->w, w->h)) continue;

        uint64_t now = timer_ticks();
        bool dbl = (w == last_click_win && now - last_click_tick <= DBL_TICKS);
        last_click_win = w;
        last_click_tick = now;

        int32_t bx, by; uint32_t bs;
        close_rect(w, &bx, &by, &bs);
        if (gui_hit(mx, my, bx, by, bs, bs)) {
            gui_window_close(w);
            return;
        }
        max_rect(w, &bx, &by, &bs);
        if (gui_hit(mx, my, bx, by, bs, bs)) {
            gui_window_focus(w);
            gui_window_maximize(w);
            return;
        }

        gui_window_focus(w);

        if (my < w->y + GUI_BORDER + GUI_TITLE_H) {   /* 标题栏 */
            if (dbl) {
                gui_window_maximize(w);
                dragging = 0;
                drag_win = 0;
                return;
            }
            dragging = 1;
            drag_win = w;
            grab_dx = mx - w->x;
            grab_dy = my - w->y;
            return;
        }
        if (w->on_click)
            w->on_click(w, mx - gui_content_x(w), my - gui_content_y(w), dbl);
        app_drag_win = w->used ? w : 0;
        return;
    }

    /* 桌面图标 */
    int ic = icon_hit(mx, my);
    if (ic >= 0) {
        uint64_t now = timer_ticks();
        bool dbl = (ic == last_icon && now - last_icon_tick <= DBL_TICKS);
        if (desk_sel != ic) {
            dmg_icon(desk_sel);
            desk_sel = ic;
            dmg_icon(ic);
        }
        last_icon = ic;
        last_icon_tick = now;
        if (dbl) {
            last_icon = -1;
            desk_icons[ic].open();
        }
    } else if (desk_sel >= 0) {
        dmg_icon(desk_sel);
        desk_sel = -1;
    }
}

/* 右键按下：菜单选择 → 应用右键回调 → 桌面菜单 */
static void mouse_rpress(void) {
    int32_t sh = (int32_t)gfx_height();

    /* 菜单已开：点在菜单上就选中，否则关掉再尝试开新菜单 */
    if (ctx_open) {
        int ci = ctx_hit(mx, my);
        if (ci >= 0) { ctx_activate(ci); return; }
        gui_context_close();
    }

    /* 右键先收起开始菜单 */
    if (start_open) {
        start_open = 0;
        menu_hover = -1;
        dmg_menu();
        return;
    }

    /* 任务栏：暂不弹菜单 */
    if (my >= sh - GUI_TASKBAR_H) return;

    /* 窗口：交给应用自己的右键回调；没有回退到通用窗口菜单（仅内容区/标题栏都可用） */
    struct gui_window *w = win_at(mx, my);
    if (w) {
        gui_window_focus(w);
        if (my >= w->y + GUI_BORDER + GUI_TITLE_H) {
            if (w->on_rclick)
                w->on_rclick(w, mx - gui_content_x(w), my - gui_content_y(w));
            else
                win_rclick(w);
        }
        return;
    }

    /* 桌面图标 */
    int ic = icon_hit(mx, my);
    if (ic >= 0) {
        if (desk_sel != ic) { dmg_icon(desk_sel); desk_sel = ic; dmg_icon(ic); }
        ctx_icon_idx = ic;
        gui_context_open(mx, my, icon_ctx_items, ICON_CTX_N,
                         desk_ctx_icon_pick, 0);
        return;
    }
    if (desk_sel >= 0) { dmg_icon(desk_sel); desk_sel = -1; }

    /* 桌面空白 */
    desk_rclick();
}

static void drag_move(void) {
    if (!drag_win || !drag_win->used) { dragging = 0; drag_win = 0; return; }
    if (drag_win->maximized) { dragging = 0; drag_win = 0; return; }

    int32_t maxx = (int32_t)gfx_width() - (int32_t)drag_win->w;
    int32_t maxy = (int32_t)(gfx_height() - GUI_TASKBAR_H) - (int32_t)drag_win->h;
    if (maxx < 0) maxx = 0;
    if (maxy < 0) maxy = 0;

    int32_t nx = mx - grab_dx, ny = my - grab_dy;
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx > maxx) nx = maxx;
    if (ny > maxy) ny = maxy;

    if (nx != drag_win->x || ny != drag_win->y) {
        dmg_window(drag_win);           /* 原位置需要还原 */
        drag_win->x = nx;
        drag_win->y = ny;
        dmg_window(drag_win);           /* 新位置需要绘制 */
    }
}

void gui_mouse_event(uint8_t buttons, int32_t dx, int32_t dy) {
    /* 登录 overlay：只把原始事件交给 login.c；不移动桌面指针、不碰窗口、
       不在这里 composite（由 desktop_tick 的 on_tick 分流统一上屏）。 */
    if (login_mouse_fn) {
        login_mouse_fn(buttons, dx, dy);
        return;
    }

    int32_t sw = (int32_t)gfx_width(), sh = (int32_t)gfx_height();

    mx += dx;
    my -= dy;                        /* PS/2 的 dy 向上为正 */
    if (mx < 0) mx = 0;
    if (mx >= sw) mx = sw - 1;
    if (my < 0) my = 0;
    if (my >= sh) my = sh - 1;

    int left = buttons & 1;
    if (left && !prev_left) mouse_press();

    /* 应用内容区拖动（框选）：按下由 on_click 启动，移动/松开在这里转发 */
    if (!left && prev_left && app_drag_win) {
        if (app_drag_win->used && app_drag_win->on_drag)
            app_drag_win->on_drag(app_drag_win,
                                  mx - gui_content_x(app_drag_win),
                                  my - gui_content_y(app_drag_win), false);
        app_drag_win = 0;
    }
    if (left && app_drag_win && app_drag_win->used && app_drag_win->on_drag)
        app_drag_win->on_drag(app_drag_win,
                              mx - gui_content_x(app_drag_win),
                              my - gui_content_y(app_drag_win), true);

    if (!left) {
        /* 松手：拖动期间被拖窗口只画了底色，这里补一帧完整内容 */
        if (dragging && drag_win && drag_win->used) dmg_window(drag_win);
        dragging = 0;
        drag_win = 0;
    }
    prev_left = left;

    int right = (buttons >> 1) & 1;      /* PS/2 包头 bit1 = 右键 */
    if (right && !prev_right) mouse_rpress();
    prev_right = right;

    update_hover();

    if (dragging) {
        drag_move();
        /* 拖动中不在中断里合成：composite 要重绘窗口并写 MMIO 帧缓冲，
           耗时常达数毫秒。IRQ12 处理一慢，i8042 的 1 字节输出缓冲就会
           溢出丢字节，鼠标包失步 —— 这是“不跟手/光标乱跳”的根因。
           这里只更新位置和脏区，真正的重绘交给主循环 gui_run() 里已有的
           if (damage_pending()) composite()。 */
        if (!damage_pending() && ptr_in_bounds(mx, my))
            gui_damage_rect(mx - 1, my - 1, ARROW_W + 2, ARROW_H + 2);
    } else if (damage_pending()) {
        composite();
    } else {
        pointer_update();
    }
}

void gui_key_event(uint16_t key) {
    /* 登录 overlay 优先吞掉键盘；不进入菜单/窗口的按键处理。 */
    if (login_key_fn) {
        login_key_fn(key);
        return;
    }

    if (key == KEY_ESC && ctx_open) {        /* 右键菜单优先被 Esc 关掉 */
        gui_context_close();
        return;
    }
    if (key == KEY_ESC && start_open) {
        start_open = 0;
        menu_hover = -1;
        dmg_menu();
        return;
    }
    if (focused && focused->used && focused->on_key)
        focused->on_key(focused, key);
}

/* ---------------- 初始化与主循环 ---------------- */
static void desktop_tick(uint64_t t) {
    /* 登录 overlay 的 tick：驱动锁屏时钟/上滑动画，然后立刻合成一帧。
       composite() 是 gui.c 内部 static，只有这里能替 login.c 上屏。 */
    if (login_tick_fn) {
        login_tick_fn(t);
        composite();
        return;
    }

    /* 只有 CPU 真的停在 hlt 里等中断时，这一 tick 才算空闲。 */
    if (idle_waiting) idle_ticks++;

    if (t % 50 == 0) {              /* 0.5s：光标闪烁 + 时钟 */
        console_blink_toggle();
        int32_t x, y; uint32_t w, h;
        if (console_cursor_rect(&x, &y, &w, &h)) gui_damage_rect(x, y, w, h);
        gui_damage_rect((int32_t)gfx_width() - 230,
                        (int32_t)gfx_height() - GUI_TASKBAR_H,
                        230, GUI_TASKBAR_H);
        if (slow_tick_fn) slow_tick_fn(t);
    }
}

void gui_init(void) {
    /* RTC 与渐变壁纸已在开机遮罩的动画循环里算好：这里只取成品，
       gui_init 不再有任何硬件等待或整屏逐行计算，老机器也不再红屏假死。 */
    if (!sysinit_rtc_time(&clk_h, &clk_m, &clk_s)) clk_h = -1;
    if (!sysinit_rtc_date(&clk_day, &clk_mon, &clk_year)) clk_day = 0;

    zcount = 0;
    focused = 0;
    hover_win = 0;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) wins[i].used = false;

    mx = (int32_t)gfx_width() / 2;
    my = (int32_t)gfx_height() / 2;

    /* 壁纸缓存必须对当前分辨率有效；不一致时 bg_cache_valid() 会同步补齐 */
    bg_cache_valid();

    gfx_set_present_hook(pointer_update);
    console_set_autopresent(0);   /* 显示交给合成器，避免逐字符整屏 present */
    composite();
    timer_set_hook(desktop_tick);
}

void gui_run(void) {
    struct input_event ev;

    for (;;) {
        /* 输入子系统负责轮询非中断型设备（xHCI）；PS/2 靠 IRQ 入队 */
        input_pump();

        while (input_poll(&ev)) {
            if (ev.type == INPUT_EV_KEY) {
                gui_key_event(ev.key);
            } else if (ev.type == INPUT_EV_MOUSE) {
                gui_mouse_event(ev.buttons, ev.dx, ev.dy);
            }
        }

        run_actions();              /* 应用投递的长任务（磁盘格式化等） */
        if (damage_pending()) composite();

        if (!input_pending()) {     /* 队列空了才允许 hlt */
            idle_waiting = 1;
            hlt();
            idle_waiting = 0;
        }
    }
}
