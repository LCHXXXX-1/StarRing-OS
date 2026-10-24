#include "sysmon.h"
#include "gfx/gui.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "hw/meminfo.h"
#include "drivers/keyboard.h"
#include "lib/string.h"

/* ============================================================
 *  系统监视器（Memory / Paging）
 *
 *  - PMM：物理页帧总量 / 空闲 / 已用 + 占用条
 *  - VMM：CR3、HHDM 偏移、经手映射页数、页表帧数
 *  - 自检按钮：走一遍 alloc frame → map → write → read → unmap → free
 *
 *  无 malloc；单文件级静态实例；回调在 IRQ 上下文（自检很短，直接同步做）。
 * ============================================================ */

#define SM_W 560
#define SM_H 400
#define SM_X 300
#define SM_Y 150

#define SM_BTN_W 220
#define SM_BTN_H 26
#define SM_BAR_H 14
#define SM_BOTTOM 20

struct sysmon {
    struct gui_window *win;
    bool     ran;              /* 是否跑过分页自检 */
    bool     passed;
    char     result[80];
};

static struct sysmon sm;

/* ---------------- 小工具 ---------------- */
static void sm_cat(char *dst, uint32_t cap, const char *s) {
    uint32_t l = 0;
    while (l + 1 < cap && dst[l]) l++;
    uint32_t i = 0;
    while (s[i] && l + 1 < cap) dst[l++] = s[i++];
    dst[l] = 0;
}

static void sm_cat_num(char *dst, uint32_t cap, uint64_t v) {
    char b[24];
    sm_cat(dst, cap, utoa(v, b, 10));
}

/* 16 位十六进制，带 0x 前缀 */
static void hex_str(uint64_t v, char *out) {
    static const char *d = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 16; i++)
        out[2 + i] = d[(v >> (60 - i * 4)) & 0xF];
    out[18] = 0;
}

/* ---------------- 自检 ---------------- */
static void sm_run_selftest(struct sysmon *s) {
    s->ran = true;
    s->result[0] = 0;
    s->passed = false;

    if (!pmm_ready()) {
        sm_cat(s->result, sizeof(s->result), "失败: PMM 未就绪");
        return;
    }
    if (!vmm_ready()) {
        sm_cat(s->result, sizeof(s->result), "失败: VMM 未就绪");
        return;
    }

    s->passed = vmm_selftest();
    sm_cat(s->result, sizeof(s->result),
           s->passed ? "通过: 分配 -> 映射 -> 读写 -> 解除映射 -> 释放。"
                     : "失败: 分页自检未通过。");
}

/* ---------------- 绘制 ---------------- */
static void sm_btn_rect(uint32_t cw, uint32_t ch,
                        int32_t *x, int32_t *y, uint32_t *w, uint32_t *h) {
    *w = SM_BTN_W;
    *h = SM_BTN_H;
    *x = ((int32_t)cw - (int32_t)SM_BTN_W) / 2;
    *y = (int32_t)ch - SM_BOTTOM - (int32_t)SM_BTN_H;
}

static void sm_draw(struct gui_window *win) {
    struct sysmon *s = win->data;
    if (!s) return;

    int32_t cx = gui_content_x(win), cy = gui_content_y(win);
    uint32_t cw = gui_content_w(win), ch = gui_content_h(win);

    gui_fill(cx, cy, cw, ch, GUI_COL_WINDOW_BG);

    char line[128], hex[20];

    /* ---------- PMM ---------- */
    gui_text_bold(cx + 16, cy + 12, "物理内存",
                  GUI_COL_ACCENT, GUI_COL_WINDOW_BG);

    uint64_t tf = pmm_total_frames(), ff = pmm_free_frames(), uf = pmm_used_frames();
    uint64_t tmb = tf * 4096 / 1048576u;
    uint64_t fmb = ff * 4096 / 1048576u;
    uint64_t umb = uf * 4096 / 1048576u;

    line[0] = 0;
    if (pmm_ready()) {
        sm_cat(line, sizeof(line), "总计 ");
        sm_cat_num(line, sizeof(line), tmb);
        sm_cat(line, sizeof(line), " MB   空闲 ");
        sm_cat_num(line, sizeof(line), fmb);
        sm_cat(line, sizeof(line), " MB   已用 ");
        sm_cat_num(line, sizeof(line), umb);
        sm_cat(line, sizeof(line), " MB   帧 ");
        sm_cat_num(line, sizeof(line), ff);
        sm_cat(line, sizeof(line), "/");
        sm_cat_num(line, sizeof(line), tf);
    } else {
        const struct mem_info *mi = meminfo_get();
        sm_cat(line, sizeof(line), "PMM 不可用。Limine 报告: ");
        sm_cat_num(line, sizeof(line), mi->usable_bytes / 1048576u);
        sm_cat(line, sizeof(line), " MB 可用 / ");
        sm_cat_num(line, sizeof(line), mi->total_bytes / 1048576u);
        sm_cat(line, sizeof(line), " MB 总计");
    }
    gui_text(cx + 16, cy + 34, line, GUI_COL_TEXT, GUI_COL_WINDOW_BG);

    /* 占用条 */
    int32_t bx = cx + 16;
    uint32_t bw = cw - 32;
    int32_t by = cy + 56;
    gui_fill(bx, by, bw, SM_BAR_H, GUI_COL_PANEL);
    gui_frame(bx, by, bw, SM_BAR_H, GUI_COL_BORDER_DARK);
    if (pmm_ready() && tf > 0) {
        uint64_t pct = uf * 100 / tf;
        uint32_t fw = (uint32_t)((uint64_t)(bw - 4) * uf / tf);
        if (fw > bw - 4) fw = bw - 4;
        if (fw > 0) gui_fill(bx + 2, by + 2, fw, SM_BAR_H - 4, GUI_COL_ACCENT);
        line[0] = 0;
        sm_cat_num(line, sizeof(line), pct);
        sm_cat(line, sizeof(line), "% 已用");
        uint32_t tw = gui_str_width(line);
        gui_text(bx + (int32_t)((bw - tw) / 2), by + (SM_BAR_H - GUI_FONT_H) / 2,
                 line, GUI_COL_TEXT, GUI_COL_PANEL);
    }

    /* ---------- VMM ---------- */
    gui_text_bold(cx + 16, cy + 86, "分页管理",
                  GUI_COL_ACCENT, GUI_COL_WINDOW_BG);

    int32_t ly = cy + 108;
    if (vmm_ready()) {
        hex_str(vmm_cr3(), hex);
        line[0] = 0;
        sm_cat(line, sizeof(line), "CR3 页表物理地址: ");
        sm_cat(line, sizeof(line), hex);
        gui_text(cx + 16, ly, line, GUI_COL_TEXT, GUI_COL_WINDOW_BG);
        ly += 18;

        hex_str(vmm_hhdm(), hex);
        line[0] = 0;
        sm_cat(line, sizeof(line), "HHDM 偏移:           ");
        sm_cat(line, sizeof(line), hex);
        gui_text(cx + 16, ly, line, GUI_COL_TEXT, GUI_COL_WINDOW_BG);
        ly += 18;

        line[0] = 0;
        sm_cat(line, sizeof(line), "VMM 已映射页: ");
        sm_cat_num(line, sizeof(line), vmm_mapped_pages());
        sm_cat(line, sizeof(line), "   页表帧: ");
        sm_cat_num(line, sizeof(line), vmm_table_frames());
        gui_text(cx + 16, ly, line, GUI_COL_TEXT, GUI_COL_WINDOW_BG);
        ly += 18;
    } else {
        gui_text(cx + 16, ly, "VMM 未初始化（无 HHDM / CR3）。",
                 GUI_COL_TEXT_DIM, GUI_COL_WINDOW_BG);
        ly += 18;
    }

    /* ---------- 自检结果 ---------- */
    ly += 10;
    if (s->ran) {
        line[0] = 0;
        sm_cat(line, sizeof(line), "自检: ");
        sm_cat(line, sizeof(line), s->result);
        gui_text(cx + 16, ly, line,
                 s->passed ? 0x007BE08A : 0x00FF6B5E, GUI_COL_WINDOW_BG);
    } else {
        gui_text(cx + 16, ly, "自检: 尚未运行。",
                 GUI_COL_TEXT_DIM, GUI_COL_WINDOW_BG);
    }

    /* ---------- 按钮 ---------- */
    int32_t bxx, byy; uint32_t bww, bhh;
    sm_btn_rect(cw, ch, &bxx, &byy, &bww, &bhh);
    bxx += cx; byy += cy;

    const char *label = "运行分页自检";
    uint32_t bg = GUI_COL_BTN;
    gui_round_fill(bxx, byy, bww, bhh, bg, GUI_COL_WINDOW_BG);
    gui_frame(bxx, byy, bww, bhh, 0x003A5C80);
    gui_pixel(bxx, byy, GUI_COL_WINDOW_BG);
    gui_pixel(bxx + (int32_t)bww - 1, byy, GUI_COL_WINDOW_BG);
    gui_text(bxx + ((int32_t)bww - (int32_t)gui_str_width(label)) / 2,
             byy + ((int32_t)bhh - GUI_FONT_H) / 2, label,
             GUI_COL_WHITE, bg);
}

/* ---------------- 交互 ---------------- */
static void sm_click(struct gui_window *win, int32_t cx, int32_t cy, bool dbl) {
    struct sysmon *s = win->data;
    if (!s) return;
    (void)dbl;

    int32_t x, y; uint32_t w, h;
    sm_btn_rect(gui_content_w(win), gui_content_h(win), &x, &y, &w, &h);
    if (!gui_hit(cx, cy, x, y, w, h)) return;

    sm_run_selftest(s);
    gui_damage();
}

static void sm_key(struct gui_window *win, uint16_t key) {
    struct sysmon *s = win->data;
    (void)win;
    if (!s) return;

    switch (key) {
    case '\n':
    case ' ':
        sm_run_selftest(s);
        gui_damage();
        return;
    case 'r':
    case 'R':
        gui_damage();
        return;
    default:
        return;
    }
}

static void sm_close(struct gui_window *win) {
    struct sysmon *s = win->data;
    if (s) s->win = 0;
}

/* ---------------- 对外接口 ---------------- */
void sysmon_open(void) {
    struct sysmon *s = &sm;

    if (s->win) {
        gui_window_focus(s->win);
        gui_damage();
        return;
    }

    s->ran = false;
    s->passed = false;
    s->result[0] = 0;

    struct gui_window *win = gui_window_open("系统监视器 - 内存",
                                             SM_X, SM_Y, SM_W, SM_H);
    if (!win) return;

    s->win = win;
    win->data = s;
    win->draw = sm_draw;
    win->on_click = sm_click;
    win->on_key = sm_key;
    win->on_close = sm_close;
    gui_window_set_icon(win, GUI_ICON_MEM);

    gui_damage();
}
