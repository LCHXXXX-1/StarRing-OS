#include "taskmgr.h"
#include "gfx/gui.h"
#include "task/task.h"
#include "mm/pmm.h"
#include "hw/meminfo.h"
#include "drivers/timer.h"
#include "drivers/keyboard.h"
#include "drivers/ahci.h"
#include "lib/string.h"

/* ============================================================
 *  任务管理器（Task Manager）—— 性能仪表盘版
 *
 *  数据源是 task/task.c 的任务登记表：系统任务在启动时登记，应用任务
 *  随 gui_window_open/close 自动登记/注销。[End Task] 通过 task_kill()
 *  关闭对应窗口（系统任务会被拒绝）。
 *
 *  CPU 采样：
 *    - 主循环空闲时进入 hlt；PIT 中断在 hlt 中到来时 gui.c 的 idle_ticks++。
 *    - 每次慢速 tick（50 tick = 500ms）取一次窗口增量：
 *          usage = (total_delta - idle_delta) * 100 / total_delta
 *      采样窗口在每次计算后重置，因此是最近 0.5s 的占用率。
 *
 *  约束：无 malloc，单文件级静态实例；周期回调运行在 timer IRQ 中，只
 *  更新静态历史和 gui_damage_rect()，绝不做全屏 gui_damage()。
 * ============================================================ */

#define TM_W 720
#define TM_H 520
#define TM_X 220
#define TM_Y 90

#define TM_DASH_H     172
#define TM_COLHDR_H   20
#define TM_ROW_H      20
#define TM_BTN_H      24
#define TM_BTN_W1     110
#define TM_BTN_W2     90
#define TM_BTN_GAP    10
#define TM_BTNBAR_H   40

#define TM_CHART_X    12
#define TM_CHART_Y    38
#define TM_CHART_W    200
#define TM_CHART_H    60
#define TM_CPU_X      224
#define TM_MEM_X      12
#define TM_MEM_Y      112
#define TM_MEM_BAR_Y  151
#define TM_MEM_BAR_W  334
#define TM_MEM_BAR_H  12
#define TM_IO_X       380
#define TM_IO_Y       27

#define TM_CPU_N      40          /* 40 * 500ms = 20 秒历史 */

enum { TB_END = 0, TB_REFRESH, TB_NONE };

struct taskmgr {
    struct gui_window *win;
    struct task_info   rows[TASK_MAX];
    int n;
    int sel;
    int top;
    int hbtn;              /* TB_*，或 TB_NONE */
    int hrow;

    /* CPU 采样窗口 */
    uint64_t last_total;
    uint64_t last_idle;
    int      cpu_pct;
    uint8_t  cpu_hist[TM_CPU_N];
    int      cpu_head;     /* 下一个写入位置 */
    int      cpu_count;
    bool     sample_valid;
};

static struct taskmgr tm;

/* ---------------- 数据 ---------------- */
static void tm_refresh(struct taskmgr *t) {
    t->n = task_snapshot(t->rows, TASK_MAX);
    if (t->sel >= t->n) t->sel = t->n ? t->n - 1 : 0;
    if (t->sel < 0) t->sel = 0;
    if (t->top < 0) t->top = 0;
}

static bool tm_selected_killable(struct taskmgr *t) {
    if (t->sel < 0 || t->sel >= t->n) return false;
    return t->rows[t->sel].kind == TASK_KIND_APP;
}

/* ---------------- 几何 ---------------- */
static void tm_btn_rect(uint32_t cw, uint32_t ch, int slot,
                        int32_t *x, int32_t *y, uint32_t *w, uint32_t *h) {
    int32_t total = TM_BTN_W1 + TM_BTN_GAP + TM_BTN_W2;
    int32_t x0 = ((int32_t)cw - total) / 2;
    *y = (int32_t)ch - TM_BTNBAR_H + (TM_BTNBAR_H - TM_BTN_H) / 2;
    *h = TM_BTN_H;
    if (slot == TB_END) { *x = x0; *w = TM_BTN_W1; }
    else                { *x = x0 + TM_BTN_W1 + TM_BTN_GAP; *w = TM_BTN_W2; }
}

static int tm_hit_btn(uint32_t cw, uint32_t ch, int32_t cx, int32_t cy) {
    for (int slot = 0; slot < 2; slot++) {
        int32_t x, y; uint32_t w, h;
        tm_btn_rect(cw, ch, slot, &x, &y, &w, &h);
        if (gui_hit(cx, cy, x, y, w, h)) return slot;
    }
    return TB_NONE;
}

/* ---------------- 绘制 ---------------- */
static void tm_draw_btn(struct taskmgr *t, int32_t cx, int32_t cy,
                        uint32_t cw, uint32_t ch, int slot) {
    int32_t x, y; uint32_t w, h;
    tm_btn_rect(cw, ch, slot, &x, &y, &w, &h);
    x += cx; y += cy;

    const char *label = (slot == TB_END) ? "结束任务" : "刷新";
    bool danger = (slot == TB_END);
    bool enabled = !danger || tm_selected_killable(t);
    bool hot = (t->hbtn == slot && enabled);

    uint32_t bg = !enabled ? GUI_COL_BTN_DISABLED
                           : danger ? (hot ? GUI_COL_CLOSE_HOVER : 0x008E2F26)
                                    : (hot ? GUI_COL_BTN_HOVER : GUI_COL_BTN);
    uint32_t bd = !enabled ? 0x00223A52
                           : danger ? 0x00F0705F : (hot ? GUI_COL_ACCENT : 0x003A5C80);

    gui_round_fill(x, y, w, h, bg, GUI_COL_WINDOW_BG);
    gui_frame(x, y, w, h, bd);
    gui_pixel(x, y, GUI_COL_WINDOW_BG);
    gui_pixel(x + (int32_t)w - 1, y, GUI_COL_WINDOW_BG);
    gui_text(x + ((int32_t)w - (int32_t)gui_str_width(label)) / 2,
             y + ((int32_t)h - GUI_FONT_H) / 2, label,
             enabled ? GUI_COL_WHITE : 0x00506A82, bg);
}

static void tm_cat(char *dst, uint32_t cap, const char *s) {
    uint32_t l = 0;
    while (l + 1 < cap && dst[l]) l++;
    uint32_t i = 0;
    while (s[i] && l + 1 < cap) dst[l++] = s[i++];
    dst[l] = 0;
}

static void tm_cat_num(char *dst, uint32_t cap, uint64_t v) {
    char b[24];
    tm_cat(dst, cap, utoa(v, b, 10));
}

/* 内存概要：优先用 PMM，不可用时退回 Limine 的总量/可用量 */
static void tm_mem_numbers(uint64_t *used_mb, uint64_t *total_mb, int *pct) {
    if (pmm_ready()) {
        uint64_t tf = pmm_total_frames(), uf = pmm_used_frames();
        *total_mb = tf * 4096 / 1048576u;
        *used_mb  = uf * 4096 / 1048576u;
        *pct = tf ? (int)(uf * 100 / tf) : 0;
        return;
    }
    const struct mem_info *mi = meminfo_get();
    *total_mb = mi->total_bytes / 1048576u;
    *used_mb  = (mi->total_bytes - mi->usable_bytes) / 1048576u;
    *pct = mi->total_bytes ? (int)((mi->total_bytes - mi->usable_bytes) * 100 /
                                   mi->total_bytes) : 0;
}

/* 1px 折线（Bresenham），只在图表小矩形内调用。 */
static void tm_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                    uint32_t color) {
    int32_t dx = x1 - x0, sx = (dx < 0) ? -1 : 1;
    if (dx < 0) dx = -dx;
    int32_t dy = y1 - y0, sy = (dy < 0) ? -1 : 1;
    if (dy < 0) dy = -dy;
    int32_t err = dx - dy;

    for (;;) {
        gui_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = err * 2;
        if (e2 >= -dy) { err -= dy; x0 += sx; }
        if (e2 <=  dx) { err += dx; y0 += sy; }
    }
}

static void tm_draw_chart(struct taskmgr *t, int32_t cx, int32_t cy) {
    int32_t gx = cx + TM_CHART_X;
    int32_t gy = cy + TM_CHART_Y;

    gui_fill(gx, gy, TM_CHART_W, TM_CHART_H, 0x000A1420);
    gui_frame(gx, gy, TM_CHART_W, TM_CHART_H, GUI_COL_BORDER_DARK);

    /* 25% / 50% / 75% 网格线 */
    for (int i = 1; i < 4; i++)
        gui_fill(gx + 1, gy + (TM_CHART_H * i) / 4,
                 TM_CHART_W - 2, 1, 0x001A2B3E);

    if (t->cpu_count <= 0) {
        gui_fill(gx + 1, gy + TM_CHART_H - 3, TM_CHART_W - 2, 1,
                 GUI_COL_TEXT_DIM);
        return;
    }

    int start = TM_CPU_N - t->cpu_count;
    int32_t px = 0, py = 0;
    bool have = false;

    for (int i = 0; i < t->cpu_count; i++) {
        int idx = (t->cpu_head - t->cpu_count + i) % TM_CPU_N;
        if (idx < 0) idx += TM_CPU_N;

        int32_t x = gx + ((start + i) * (TM_CHART_W - 1)) / (TM_CPU_N - 1);
        int32_t y = gy + (TM_CHART_H - 3) -
                    ((int32_t)t->cpu_hist[idx] * (TM_CHART_H - 6)) / 100;
        if (have) tm_line(px, py, x, y, GUI_COL_ACCENT);
        else      gui_pixel(x, y, GUI_COL_ACCENT);
        px = x; py = y; have = true;
    }

    /* 最新采样点用小方块强调（保持在图表边框内） */
    gui_fill(px - 1, py - 1, 2, 2, GUI_COL_WHITE);
}

static void tm_draw_summary(struct taskmgr *t, int32_t cx, int32_t cy,
                            uint32_t cw) {
    gui_fill(cx, cy, cw, TM_DASH_H, GUI_COL_PANEL);
    gui_fill(cx, cy + TM_DASH_H - 1, cw, 1, GUI_COL_BORDER_DARK);

    tm_draw_chart(t, cx, cy);

    /* CPU 占用率文本 */
    char pct[24];
    pct[0] = 0;
    tm_cat_num(pct, sizeof(pct), (uint64_t)t->cpu_pct);
    tm_cat(pct, sizeof(pct), "%");
    gui_text(cx + TM_CPU_X, cy + 30, "处理器使用率", GUI_COL_TEXT_DIM,
             GUI_COL_PANEL);
    gui_text_bold(cx + TM_CPU_X, cy + 50, pct, GUI_COL_WHITE, GUI_COL_PANEL);
    gui_text(cx + TM_CPU_X, cy + 74, "最近 20 秒", GUI_COL_TEXT_DIM,
             GUI_COL_PANEL);

    /* 内存动态条 */
    uint64_t used_mb, total_mb; int mpct;
    tm_mem_numbers(&used_mb, &total_mb, &mpct);

    char line[128];
    line[0] = 0;
    tm_cat(line, sizeof(line), "已用: ");
    tm_cat_num(line, sizeof(line), used_mb);
    tm_cat(line, sizeof(line), " MB / 总计: ");
    tm_cat_num(line, sizeof(line), total_mb);
    tm_cat(line, sizeof(line), " MB");
    gui_text(cx + TM_MEM_X, cy + TM_MEM_Y, "内存", GUI_COL_TEXT_DIM,
             GUI_COL_PANEL);
    gui_text(cx + TM_MEM_X, cy + TM_MEM_Y + 18, line, GUI_COL_TEXT,
             GUI_COL_PANEL);

    int32_t bx = cx + TM_MEM_X;
    int32_t by = cy + TM_MEM_BAR_Y;
    gui_fill(bx, by, TM_MEM_BAR_W, TM_MEM_BAR_H, GUI_COL_WINDOW_BG);
    gui_frame(bx, by, TM_MEM_BAR_W, TM_MEM_BAR_H, GUI_COL_BORDER_DARK);
    uint32_t fw = (uint32_t)((uint64_t)(TM_MEM_BAR_W - 4) *
                             (uint64_t)mpct / 100u);
    if (fw > TM_MEM_BAR_W - 4) fw = TM_MEM_BAR_W - 4;
    if (fw > 0) gui_fill(bx + 2, by + 2, fw, TM_MEM_BAR_H - 4, GUI_COL_ACCENT);

    pct[0] = 0;
    tm_cat_num(pct, sizeof(pct), (uint64_t)mpct);
    tm_cat(pct, sizeof(pct), "%");
    gui_text(bx + TM_MEM_BAR_W + 10, by - 2, pct, GUI_COL_TEXT_DIM,
             GUI_COL_PANEL);

    /* 磁盘 I/O（AHCI 累计扇区数） */
    gui_text(cx + TM_IO_X, cy + TM_IO_Y, "磁盘 I/O", GUI_COL_TEXT_DIM,
             GUI_COL_PANEL);

    uint64_t rd = read_sectors_total, wr = write_sectors_total;
    line[0] = 0;
    tm_cat(line, sizeof(line), "读:  ");
    tm_cat_num(line, sizeof(line), rd);
    tm_cat(line, sizeof(line), " 扇区 (");
    tm_cat_num(line, sizeof(line), rd * 512u / 1048576u);
    tm_cat(line, sizeof(line), " MB)");
    gui_text(cx + TM_IO_X, cy + TM_IO_Y + 22, line, GUI_COL_TEXT,
             GUI_COL_PANEL);

    line[0] = 0;
    tm_cat(line, sizeof(line), "写: ");
    tm_cat_num(line, sizeof(line), wr);
    tm_cat(line, sizeof(line), " 扇区 (");
    tm_cat_num(line, sizeof(line), wr * 512u / 1048576u);
    tm_cat(line, sizeof(line), " MB)");
    gui_text(cx + TM_IO_X, cy + TM_IO_Y + 42, line, GUI_COL_TEXT,
             GUI_COL_PANEL);

    line[0] = 0;
    tm_cat(line, sizeof(line), "运行时间: ");
    tm_cat_num(line, sizeof(line), timer_seconds());
    tm_cat(line, sizeof(line), " s");
    gui_text(cx + TM_IO_X, cy + TM_IO_Y + 66, line, GUI_COL_TEXT_DIM,
             GUI_COL_PANEL);
}

static void tm_draw(struct gui_window *win) {
    struct taskmgr *t = win->data;
    if (!t) return;

    tm_refresh(t);

    int32_t cx = gui_content_x(win), cy = gui_content_y(win);
    uint32_t cw = gui_content_w(win), ch = gui_content_h(win);

    gui_fill(cx, cy, cw, ch, GUI_COL_WINDOW_BG);
    tm_draw_summary(t, cx, cy, cw);

    /* 列头 */
    int32_t hy = cy + TM_DASH_H;
    gui_fill(cx, hy, cw, TM_COLHDR_H, GUI_COL_PANEL_LIGHT);
    gui_fill(cx, hy + TM_COLHDR_H - 1, cw, 1, GUI_COL_BORDER_DARK);
    int32_t hty = hy + (TM_COLHDR_H - GUI_FONT_H) / 2;
    int32_t col_type = cx + (int32_t)cw - 215;
    int32_t col_cpu  = cx + (int32_t)cw - 140;
    int32_t col_up   = cx + (int32_t)cw - 72;
    gui_text(cx + 12, hty, "进程号", GUI_COL_TEXT_DIM, GUI_COL_PANEL_LIGHT);
    gui_text(cx + 64, hty, "名称", GUI_COL_TEXT_DIM, GUI_COL_PANEL_LIGHT);
    gui_text(col_type, hty, "类型", GUI_COL_TEXT_DIM, GUI_COL_PANEL_LIGHT);
    gui_text(col_cpu, hty, "处理器", GUI_COL_TEXT_DIM, GUI_COL_PANEL_LIGHT);
    gui_text(col_up, hty, "运行时间", GUI_COL_TEXT_DIM, GUI_COL_PANEL_LIGHT);

    /* 列表 */
    int32_t list_y = cy + TM_DASH_H + TM_COLHDR_H;
    uint32_t list_h = ch - TM_DASH_H - TM_COLHDR_H - TM_BTNBAR_H;
    int visible = (int)(list_h / TM_ROW_H);
    if (visible < 1) visible = 1;

    if (t->sel < t->top) t->top = t->sel;
    if (t->sel >= t->top + visible) t->top = t->sel - visible + 1;
    if (t->top < 0) t->top = 0;

    for (int i = 0; i < visible; i++) {
        int idx = t->top + i;
        int32_t ry = list_y + i * TM_ROW_H;
        if (idx >= t->n) {
            gui_fill(cx, ry, cw, TM_ROW_H, GUI_COL_WINDOW_BG);
            continue;
        }
        struct task_info *r = &t->rows[idx];
        bool sel = (idx == t->sel);
        bool hot = (idx == t->hrow);
        uint32_t bg = sel ? GUI_COL_SELECT
                          : (hot ? GUI_COL_SELECT_HOVER
                                 : ((i & 1) ? GUI_COL_ROW_ALT : GUI_COL_WINDOW_BG));
        gui_fill(cx, ry, cw, TM_ROW_H, bg);
        if (sel) gui_fill(cx, ry, 3, TM_ROW_H, GUI_COL_ACCENT);

        char num[24];
        gui_text(cx + 12, ry + 2, utoa((uint64_t)r->pid, num, 10),
                 GUI_COL_TEXT_DIM, bg);

        int maxc = (int)(((int32_t)cw - 64 - 230) / GUI_FONT_W);
        if (maxc < 4) maxc = 4;
        gui_text_max(cx + 64, ry + 2, r->name, maxc, GUI_COL_TEXT, bg);

        bool app = (r->kind == TASK_KIND_APP);
        gui_text(col_type, ry + 2, app ? "应用" : "系统",
                 app ? GUI_COL_ACCENT : GUI_COL_TEXT_DIM, bg);

        /* 单核协作式系统没有独立的任务时间片，CPU 列显示 N/A。 */
        gui_text(col_cpu, ry + 2, "不可用", GUI_COL_TEXT_DIM, bg);

        uint64_t up = timer_ticks();
        uint64_t secs = (up >= r->start_tick) ? (up - r->start_tick) / 100u : 0;
        gui_text(col_up, ry + 2, utoa(secs, num, 10), GUI_COL_TEXT_DIM, bg);
        gui_text(col_up + (int32_t)gui_str_width(utoa(secs, num, 10)),
                 ry + 2, "秒", GUI_COL_TEXT_DIM, bg);
    }

    if (t->n == 0)
        gui_text(cx + 12, list_y + 4, "(无任务)", GUI_COL_TEXT_DIM,
                 GUI_COL_WINDOW_BG);

    /* 底部按钮条 */
    int32_t bar_y = (int32_t)(cy + ch - TM_BTNBAR_H);
    gui_fill(cx, bar_y, cw, TM_BTNBAR_H, GUI_COL_PANEL);
    gui_fill(cx, bar_y, cw, 1, GUI_COL_BORDER_DARK);

    if (t->sel >= 0 && t->sel < t->n) {
        char info[96];
        info[0] = 0;
        tm_cat(info, sizeof(info), "已选: ");
        tm_cat(info, sizeof(info), t->rows[t->sel].name);
        if (!tm_selected_killable(t))
            tm_cat(info, sizeof(info), "  (系统任务 - 无法结束)");
        gui_text(cx + 12, bar_y + (TM_BTNBAR_H - GUI_FONT_H) / 2, info,
                 GUI_COL_TEXT_DIM, GUI_COL_PANEL);
    }

    tm_draw_btn(t, cx, cy, cw, ch, TB_END);
    tm_draw_btn(t, cx, cy, cw, ch, TB_REFRESH);
}

/* ---------------- 周期采样 ---------------- */
static void tm_sample(struct taskmgr *t, uint64_t now) {
    uint64_t idle = gui_idle_ticks();

    if (t->sample_valid) {
        uint64_t dt = (now >= t->last_total) ? now - t->last_total : 0;
        uint64_t di = (idle >= t->last_idle) ? idle - t->last_idle : 0;
        int usage = t->cpu_pct;

        if (dt == 0) {
            usage = t->cpu_pct;
        } else if (di >= dt) {
            usage = 0;
        } else {
            usage = (int)(((dt - di) * 100u) / dt);
        }
        if (usage < 0) usage = 0;
        if (usage > 100) usage = 100;
        t->cpu_pct = usage;

        t->cpu_hist[t->cpu_head] = (uint8_t)usage;
        t->cpu_head = (t->cpu_head + 1) % TM_CPU_N;
        if (t->cpu_count < TM_CPU_N) t->cpu_count++;
    }

    t->last_total = now;
    t->last_idle = idle;
    t->sample_valid = true;
}

static void tm_tick(uint64_t ticks) {
    struct taskmgr *t = &tm;
    if (!t->win || !t->win->used) return;

    tm_sample(t, ticks);

    int32_t cx = gui_content_x(t->win), cy = gui_content_y(t->win);
    uint32_t cw = gui_content_w(t->win);

    /* 只标记仪表盘里的几个小矩形；绝不做全屏 gui_damage()。 */
    gui_damage_rect(cx + TM_CHART_X - 1, cy + TM_CHART_Y - 1,
                    TM_CHART_W + 2, TM_CHART_H + 2);
    gui_damage_rect(cx + TM_CPU_X, cy + 26, 150, 66);
    gui_damage_rect(cx + TM_MEM_X, cy + TM_MEM_Y,
                    TM_MEM_BAR_W + 55, TM_MEM_BAR_Y - TM_MEM_Y + 20);
    if ((int32_t)cw > TM_IO_X + 20)
        gui_damage_rect(cx + TM_IO_X, cy + TM_IO_Y,
                        cw - TM_IO_X - 12, 100);
}

/* ---------------- 交互 ---------------- */
static void tm_end_selected(struct taskmgr *t) {
    if (t->sel < 0 || t->sel >= t->n) return;
    if (t->rows[t->sel].kind != TASK_KIND_APP) return;
    task_kill(t->rows[t->sel].pid);
    tm_refresh(t);
    gui_damage();
}

static void tm_click(struct gui_window *win, int32_t cx, int32_t cy, bool dbl) {
    struct taskmgr *t = win->data;
    if (!t) return;
    (void)dbl;
    tm_refresh(t);

    uint32_t cw = gui_content_w(win), ch = gui_content_h(win);
    int slot = tm_hit_btn(cw, ch, cx, cy);
    if (slot == TB_END)     { tm_end_selected(t); return; }
    if (slot == TB_REFRESH) { tm_refresh(t); gui_damage(); return; }
    if (slot != TB_NONE)    return;

    int32_t list_y = TM_DASH_H + TM_COLHDR_H;
    if (cy < list_y) return;
    if (cy >= (int32_t)(ch - TM_BTNBAR_H)) return;
    int row = (cy - list_y) / TM_ROW_H;
    int idx = t->top + row;
    if (idx < 0 || idx >= t->n) return;
    t->sel = idx;
    gui_damage();
}

static void tm_hover(struct gui_window *win, int32_t cx, int32_t cy) {
    struct taskmgr *t = win->data;
    if (!t) return;

    int btn = TB_NONE, row = -1;
    if (cx >= 0 && cy >= 0) {
        btn = tm_hit_btn(gui_content_w(win), gui_content_h(win), cx, cy);
        if (btn == TB_NONE && cy >= TM_DASH_H + TM_COLHDR_H &&
            cy < (int32_t)(gui_content_h(win) - TM_BTNBAR_H)) {
            int r = (cy - (TM_DASH_H + TM_COLHDR_H)) / TM_ROW_H;
            int idx = t->top + r;
            if (idx >= 0 && idx < t->n) row = idx;
        }
    }
    bool changed = false;
    if (btn != t->hbtn) { t->hbtn = btn; changed = true; }
    if (row != t->hrow) { t->hrow = row; changed = true; }
    if (changed) gui_damage();
}

static void tm_key(struct gui_window *win, uint16_t key) {
    struct taskmgr *t = win->data;
    if (!t) return;
    tm_refresh(t);

    switch (key) {
    case KEY_UP:   if (t->sel > 0) t->sel--; break;
    case KEY_DOWN: if (t->sel + 1 < t->n) t->sel++; break;
    case KEY_PGUP: t->sel -= 10; if (t->sel < 0) t->sel = 0; break;
    case KEY_PGDN:
        t->sel += 10;
        if (t->sel >= t->n) t->sel = t->n ? t->n - 1 : 0;
        break;
    case KEY_HOME: t->sel = 0; break;
    case KEY_END:  t->sel = t->n ? t->n - 1 : 0; break;
    case KEY_DELETE:
    case '\n':
        tm_end_selected(t);
        return;
    case 'r':
    case 'R':
        tm_refresh(t);
        break;
    case KEY_ESC:
        gui_window_close(win);
        return;
    default:
        return;
    }
    gui_damage();
}

static void tm_close(struct gui_window *win) {
    struct taskmgr *t = win->data;
    if (t) t->win = 0;
    gui_set_slow_tick(0);
}

/* ---------------- 对外接口 ---------------- */
void taskmgr_open(void) {
    struct taskmgr *t = &tm;

    if (t->win) {                     /* 已打开：聚焦刷新 */
        gui_window_focus(t->win);
        tm_refresh(t);
        gui_damage();
        return;
    }

    for (int i = 0; i < TASK_MAX; i++) t->rows[i].used = false;
    t->n = 0; t->sel = 0; t->top = 0;
    t->hbtn = TB_NONE; t->hrow = -1;
    t->last_total = timer_ticks();
    t->last_idle = gui_idle_ticks();
    t->cpu_pct = 0;
    for (int i = 0; i < TM_CPU_N; i++) t->cpu_hist[i] = 0;
    t->cpu_head = 0;
    t->cpu_count = TM_CPU_N;      /* 初始画一条满宽的 0% 基线 */
    t->sample_valid = true;

    struct gui_window *win = gui_window_open("任务管理器", TM_X, TM_Y,
                                             TM_W, TM_H);
    if (!win) return;

    t->win = win;
    win->data = t;
    win->draw = tm_draw;
    win->on_click = tm_click;
    win->on_hover = tm_hover;
    win->on_key = tm_key;
    win->on_close = tm_close;
    gui_window_set_icon(win, GUI_ICON_TASKS);

    gui_set_slow_tick(tm_tick);
    tm_refresh(t);
    gui_damage();
}
