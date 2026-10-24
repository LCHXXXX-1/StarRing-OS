#include "sysprop.h"
#include "gfx/gui.h"
#include "arch/x86_64/cpuinfo.h"
#include "hw/meminfo.h"
#include "lib/string.h"
#include "gfx/gfx.h"

/* ============================================================
 *  系统属性窗口（静态单例）
 *
 *  - 400x280，打开时居中（gui_window_open 会再夹一次屏幕边界）
 *  - 内容：版本 / 处理器 / 核心数 / 内存 / 内核
 *  - 无 malloc、无回调状态机；重复打开只聚焦
 * ============================================================ */

#define SP_W 400
#define SP_H 280

struct sysprop {
    struct gui_window *win;
};

static struct sysprop sp;

/* 有界字符串追加 */
static void sp_cat(char *dst, uint32_t cap, const char *s) {
    uint32_t n = 0;
    while (n + 1 < cap && dst[n]) n++;
    while (*s && n + 1 < cap) dst[n++] = *s++;
    dst[n] = 0;
}

static void sp_num(char *dst, uint32_t cap, uint64_t v) {
    char b[24];
    sp_cat(dst, cap, utoa(v, b, 10));
}

static void sp_draw(struct gui_window *win) {
    int32_t  cx = gui_content_x(win), cy = gui_content_y(win);
    uint32_t cw = gui_content_w(win), ch = gui_content_h(win);

    gui_fill(cx, cy, cw, ch, GUI_COL_WINDOW_BG);

    const struct cpu_info *cpu = cpuinfo_get();
    const struct mem_info *mem = meminfo_get();

    char buf[96];
    int32_t y = cy + 18;

    gui_text_bold(cx + 20, y, "StarRing OS 0.1.0",
                  GUI_COL_ACCENT, GUI_COL_WINDOW_BG);
    y += 26;
    gui_fill(cx + 16, y, cw - 32, 1, GUI_COL_BORDER_DARK);
    y += 16;

    buf[0] = 0;
    sp_cat(buf, sizeof(buf), "处理器: ");
    sp_cat(buf, sizeof(buf), (cpu && cpu->brand[0]) ? cpu->brand : "未知");
    gui_text(cx + 20, y, buf, GUI_COL_TEXT, GUI_COL_WINDOW_BG);
    y += 26;

    buf[0] = 0;
    sp_cat(buf, sizeof(buf), "核心数: ");
    sp_num(buf, sizeof(buf), cpu ? (uint64_t)cpu->logical_cores : 0);
    gui_text(cx + 20, y, buf, GUI_COL_TEXT, GUI_COL_WINDOW_BG);
    y += 26;

    buf[0] = 0;
    sp_cat(buf, sizeof(buf), "内存: ");
    sp_num(buf, sizeof(buf), mem ? mem->total_bytes / 1048576u : 0);
    sp_cat(buf, sizeof(buf), " MB");
    gui_text(cx + 20, y, buf, GUI_COL_TEXT, GUI_COL_WINDOW_BG);
    y += 26;

    buf[0] = 0;
    sp_cat(buf, sizeof(buf), "内核: SRCore");
    gui_text(cx + 20, y, buf, GUI_COL_TEXT, GUI_COL_WINDOW_BG);
    y += 26;

    gui_fill(cx + 16, y, cw - 32, 1, GUI_COL_BORDER_DARK);
    y += 14;
    gui_text(cx + 20, y, "StarRing OS · Ring Architecture",
             GUI_COL_TEXT_DIM, GUI_COL_WINDOW_BG);
}

static void sp_close(struct gui_window *win) {
    struct sysprop *s = win->data;
    if (s) s->win = 0;
}

void sysprop_open(void) {
    struct sysprop *s = &sp;

    if (s->win) {                    /* 防重复打开 */
        gui_window_focus(s->win);
        return;
    }

    int32_t x = ((int32_t)gfx_width() - SP_W) / 2;
    int32_t y = ((int32_t)gfx_height() - GUI_TASKBAR_H - SP_H) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    struct gui_window *win = gui_window_open("系统属性", x, y, SP_W, SP_H);
    if (!win) return;

    s->win = win;
    win->data = s;
    win->draw = sp_draw;
    win->on_close = sp_close;
    gui_window_set_icon(win, GUI_ICON_PC);

    gui_damage();
}
