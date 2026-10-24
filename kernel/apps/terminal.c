#include "terminal.h"
#include "gfx/gui.h"
#include "gfx/console.h"
#include "shell/shell.h"
#include "files.h"
#include "fs/fs.h"
#include "drivers/keyboard.h"
#include "lib/string.h"

#define TERM_W   900
#define TERM_H   520
#define TERM_X   170
#define TERM_Y   70

#define LINE_MAX 120
#define HIST_MAX 8

static const char *PROMPT = "StarRing OS /> ";

static struct gui_window *term_win = 0;
static int  console_viewed = 0;        /* 是否已给 console 设置过视图 */

static char     line[LINE_MAX + 1];
static uint32_t len = 0;
static uint32_t pos = 0;
static uint32_t shown = 0;

static uint32_t in_col = 0, in_row = 0;

static char     hist[HIST_MAX][LINE_MAX + 1];
static uint32_t hist_count = 0;
static int      hist_browse = -1;
static char     hist_stash[LINE_MAX + 1];

/* 把 console 的内容脏区转成 GUI 局部重绘请求 */
static void term_flush_damage(void) {
    int32_t dx, dy; uint32_t dw, dh;
    if (console_take_dirty(&dx, &dy, &dw, &dh))
        gui_damage_rect(dx, dy, dw, dh);
}

static void redraw_line(void) {
    char blank[LINE_MAX + 1];
    for (uint32_t i = 0; i < shown; i++) blank[i] = ' ';
    blank[shown] = 0;
    if (shown) console_write_at(in_col, in_row, blank);
    console_write_at(in_col, in_row, line);
    shown = len;
    console_set_cursor(in_col + pos, in_row);
}

static void history_add(const char *s) {
    if (!s[0]) return;
    if (hist_count && kstrcmp(hist[hist_count - 1], s) == 0) return;
    if (hist_count == HIST_MAX) {
        for (uint32_t i = 1; i < HIST_MAX; i++)
            kstrcpy(hist[i - 1], hist[i]);
        hist_count--;
    }
    kstrcpy(hist[hist_count++], s);
}

static void history_load(int idx) {
    if (idx < 0 || idx >= (int)hist_count) return;
    kstrcpy(line, hist[idx]);
    len = (uint32_t)kstrlen(line);
    pos = len;
    redraw_line();
}

static void submit_line(void) {
    console_putc('\n');
    if (len) history_add(line);
    shell_execute(line);
    files_refresh_all();
    terminal_prompt();
    term_flush_damage();
}

static void term_key(struct gui_window *win, uint16_t k) {
    (void)win;

    switch (k) {
    case '\n':
        submit_line();
        break;

    case '\b':                                  /* Backspace */
        if (pos > 0) {
            for (uint32_t i = pos; i < len; i++) line[i - 1] = line[i];
            len--; pos--; line[len] = 0;
            redraw_line();
        }
        break;

    case KEY_LEFT:
        if (pos > 0) { pos--; console_set_cursor(in_col + pos, in_row); }
        break;

    case KEY_RIGHT:
        if (pos < len) { pos++; console_set_cursor(in_col + pos, in_row); }
        break;

    case KEY_HOME:
        pos = 0; console_set_cursor(in_col, in_row);
        break;

    case KEY_END:
        pos = len; console_set_cursor(in_col + pos, in_row);
        break;

    case KEY_DELETE:
        if (pos < len) {
            for (uint32_t i = pos; i + 1 < len; i++) line[i] = line[i + 1];
            len--; line[len] = 0;
            redraw_line();
        }
        break;

    case KEY_UP:
        if (hist_count == 0) break;
        if (hist_browse < 0) {
            kstrcpy(hist_stash, line);
            hist_browse = (int)hist_count - 1;
        } else if (hist_browse > 0) {
            hist_browse--;
        }
        history_load(hist_browse);
        break;

    case KEY_DOWN:
        if (hist_browse < 0) break;
        if (hist_browse + 1 < (int)hist_count) {
            history_load(++hist_browse);
        } else {
            hist_browse = -1;
            kstrcpy(line, hist_stash);
            len = (uint32_t)kstrlen(line);
            pos = len;
            redraw_line();
        }
        break;

    default:                                    /* 可打印字符插入光标处 */
        if (k >= 32 && k < 127 && len < LINE_MAX) {
            for (uint32_t i = len; i > pos; i--) line[i] = line[i - 1];
            line[pos] = (char)k;
            len++; pos++; line[len] = 0;
            redraw_line();
        }
        break;
    }

    term_flush_damage();
}

static void term_draw(struct gui_window *win) {
    console_set_origin(gui_content_x(win), gui_content_y(win));
    console_render();
}

static void term_resize(struct gui_window *win) {
    console_set_origin(gui_content_x(win), gui_content_y(win));
    console_set_view_size(gui_content_w(win), gui_content_h(win));
    term_flush_damage();
    gui_damage();
}

static void term_close(struct gui_window *win) {
    (void)win;
    /* 把 console 视图挪到屏幕外，避免光标继续画到别的窗口上 */
    console_set_origin(-20000, -20000);
    term_win = 0;
}

void terminal_prompt(void) {
    console_puts(PROMPT);
    console_get_cursor(&in_col, &in_row);
    len = 0; pos = 0; shown = 0;
    line[0] = 0;
    hist_browse = -1;
    term_flush_damage();
}

void terminal_open(void) {
    if (term_win && term_win->used) {
        gui_window_focus(term_win);
        return;
    }

    struct gui_window *win = gui_window_open("终端", TERM_X, TERM_Y,
                                             TERM_W, TERM_H);
    if (!win) return;

    term_win = win;
    win->data = 0;
    win->draw = term_draw;
    win->on_key = term_key;
    win->on_resize = term_resize;
    win->on_close = term_close;
    gui_window_set_icon(win, GUI_ICON_TERM);

    if (!console_viewed) {
        console_set_view(gui_content_x(win), gui_content_y(win),
                         gui_content_w(win), gui_content_h(win));
        console_viewed = 1;
    } else {
        console_set_origin(gui_content_x(win), gui_content_y(win));
    }
    gui_damage();
}

void terminal_open_at(const char *path) {
    bool existed = (term_win && term_win->used);
    terminal_open();
    if (!term_win || !term_win->used) return;

    /* 已打开的终端：结束当前输入行并丢弃未提交的草稿。 */
    if (existed) console_putc('\n');
    line[0] = 0;
    len = 0;
    pos = 0;
    shown = 0;
    hist_browse = -1;

    if (path && path[0]) {
        int r = fs_chdir(path);
        if (r < 0) {
            console_puts("cd: ");
            console_puts(path);
            console_puts(": ");
            console_puts(fs_strerror(r));
            console_putc('\n');
        }
    }

    terminal_prompt();
    term_flush_damage();
}
