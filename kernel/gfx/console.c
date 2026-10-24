#include "console.h"
#include "gfx.h"
#include "font.h"
#include "font_ttf.h"

#define FG 0x00D8E2EE
#define BG 0x000A121E
#define CURSOR_COLOR 0x00FFFFFF
#define CURSOR_H 2   /* 下划线光标高度（像素） */

#define CONSOLE_MAX_COLS 128
#define CONSOLE_MAX_ROWS 50

/* 字符网格：每格存一个 Unicode 码点（不再是单个字节）。
   双宽字（CJK）占两格：左格存码点，右格存 CELL_CONT 占位。 */
#define CELL_CONT 0xFFFFFFFFu

static uint32_t grid[CONSOLE_MAX_ROWS][CONSOLE_MAX_COLS];
static uint32_t cx = 0, cy = 0;
static uint32_t cols = 0, rows = 0;      /* 视图可容纳的字符格数 */
static int32_t  org_x = 0, org_y = 0;    /* 视图左上角像素（窗口内容区） */
static uint32_t view_w = 0, view_h = 0;  /* 视图像素尺寸 */

/* UTF-8 续字节累积（console_puts 是按字节喂进来的） */
static char    u8_pend[5];
static uint8_t u8_need = 0, u8_have = 0;

static int      blink_on  = 1;   /* 逻辑上光标当前是否应显示 */
static int      cursor_drawn = 0;
static int      put_depth = 0;   /* console_puts 嵌套深度：>0 时延后 present */
static int      autopresent = 1; /* GUI 接管后置 0 */
static uint32_t saved_px[FONT_WIDTH * CURSOR_H]; /* 光标覆盖的原始像素 */

/* 内容脏区（字符格，闭区间）：GUI 局部重绘用 */
static uint32_t dty_x0, dty_y0, dty_x1, dty_y1;
static int      dty_any = 0;

static void dty_mark(uint32_t x, uint32_t y) {
    if (!dty_any) {
        dty_x0 = dty_x1 = x;
        dty_y0 = dty_y1 = y;
        dty_any = 1;
        return;
    }
    if (x < dty_x0) dty_x0 = x;
    if (x > dty_x1) dty_x1 = x;
    if (y < dty_y0) dty_y0 = y;
    if (y > dty_y1) dty_y1 = y;
}

static void dty_all(void) {
    if (cols == 0 || rows == 0) { dty_any = 0; return; }
    dty_x0 = 0; dty_y0 = 0;
    dty_x1 = cols - 1; dty_y1 = rows - 1;
    dty_any = 1;
}

void console_set_autopresent(int on) { autopresent = on ? 1 : 0; }

bool console_take_dirty(int32_t *x, int32_t *y, uint32_t *w, uint32_t *h) {
    if (!dty_any || cols == 0 || rows == 0) { dty_any = 0; return false; }
    if (x) *x = org_x + (int32_t)(dty_x0 * FONT_WIDTH);
    if (y) *y = org_y + (int32_t)(dty_y0 * FONT_HEIGHT);
    if (w) *w = (dty_x1 - dty_x0 + 1) * FONT_WIDTH;
    if (h) *h = (dty_y1 - dty_y0 + 1) * FONT_HEIGHT;
    dty_any = 0;
    return true;
}

bool console_cursor_rect(int32_t *x, int32_t *y, uint32_t *w, uint32_t *h) {
    if (cols == 0 || rows == 0) return false;
    if (x) *x = org_x + (int32_t)(cx * FONT_WIDTH);
    if (y) *y = org_y + (int32_t)(cy * FONT_HEIGHT);
    if (w) *w = FONT_WIDTH;
    if (h) *h = FONT_HEIGHT;
    return true;
}

/* 光标始终位于字符格 (cx, cy) 的底部两行 */
static void cursor_save(void) {
    int32_t px = org_x + (int32_t)(cx * FONT_WIDTH);
    int32_t py = org_y + (int32_t)((cy + 1) * FONT_HEIGHT) - CURSOR_H;
    for (uint32_t i = 0; i < FONT_WIDTH; i++)
        for (uint32_t j = 0; j < CURSOR_H; j++)
            saved_px[i * CURSOR_H + j] = gfx_get_pixel(px + (int32_t)i,
                                                       py + (int32_t)j);
}

static void draw_cursor(void) {
    if (cursor_drawn || !blink_on) return;
    if (cols == 0 || rows == 0) return;
    cursor_save();
    dty_mark(cx, cy);
    int32_t px = org_x + (int32_t)(cx * FONT_WIDTH);
    int32_t py = org_y + (int32_t)((cy + 1) * FONT_HEIGHT) - CURSOR_H;
    for (uint32_t i = 0; i < FONT_WIDTH; i++)
        for (uint32_t j = 0; j < CURSOR_H; j++)
            gfx_pixel(px + (int32_t)i, py + (int32_t)j, CURSOR_COLOR);
    cursor_drawn = 1;
}

/* 恢复光标覆盖的像素，可在文本中间位置安全擦除 */
static void erase_cursor(void) {
    if (!cursor_drawn) return;
    dty_mark(cx, cy);
    int32_t px = org_x + (int32_t)(cx * FONT_WIDTH);
    int32_t py = org_y + (int32_t)((cy + 1) * FONT_HEIGHT) - CURSOR_H;
    for (uint32_t i = 0; i < FONT_WIDTH; i++)
        for (uint32_t j = 0; j < CURSOR_H; j++)
            gfx_pixel(px + (int32_t)i, py + (int32_t)j,
                      saved_px[i * CURSOR_H + j]);
    cursor_drawn = 0;
}

/* 码点 -> UTF-8 字节（返回字节数，末尾补 0） */
static int cp_encode(uint32_t cp, char *s) {
    int n = 0;
    if (cp < 0x80) {
        s[n++] = (char)cp;
    } else if (cp < 0x800) {
        s[n++] = (char)(0xC0 | (cp >> 6));
        s[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s[n++] = (char)(0xE0 | (cp >> 12));
        s[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        s[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        s[n++] = (char)(0xF0 | (cp >> 18));
        s[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        s[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        s[n++] = (char)(0x80 | (cp & 0x3F));
    }
    s[n] = 0;
    return n;
}

/* 码点在固定 8px 字符网格里占几格：CJK 双宽，其余单宽 */
static int cp_width(uint32_t cp) {
    if (cp < 0x80) return 1;
    char s[5];
    cp_encode(cp, s);
    if (ttf_available()) return ttf_get_text_width(s) >= 12 ? 2 : 1;
    return cp >= 0x2E80 ? 2 : 1;      /* 无字体时按区块判断，避免整行错位 */
}

static void cell_bg(uint32_t x, uint32_t y, uint32_t w) {
    gfx_rect(org_x + (int32_t)(x * FONT_WIDTH),
             org_y + (int32_t)(y * FONT_HEIGHT),
             w * FONT_WIDTH, FONT_HEIGHT, BG);
}

/* 擦掉一格（宽字连同右半格） */
static void clear_cell(uint32_t x, uint32_t y) {
    if (x >= cols || y >= rows) return;
    int w = (x + 1 < cols && grid[y][x + 1] == CELL_CONT) ? 2 : 1;
    cell_bg(x, y, (uint32_t)w);
    grid[y][x] = ' ';
    if (w > 1) grid[y][x + 1] = ' ';
    dty_mark(x, y);
    if (w > 1) dty_mark(x + 1, y);
}

/* 在网格 (x, y) 写入一个码点并立即绘制字形 */
static void plot_cp(uint32_t x, uint32_t y, uint32_t cp) {
    if (x >= cols || y >= rows) return;
    int w = cp_width(cp);
    if (w > 1 && x + 1 >= cols) w = 1;      /* 宽字不跨行 */

    /* 覆盖到旧的双宽字模时整格擦除：屏幕上绝不能留下半个字 */
    if (grid[y][x] == CELL_CONT && x > 0) {
        cell_bg(x - 1, y, 2);
        grid[y][x - 1] = ' ';
        grid[y][x] = ' ';
        dty_mark(x - 1, y);
    } else if (grid[y][x] != CELL_CONT && x + 1 < cols &&
               grid[y][x + 1] == CELL_CONT) {
        cell_bg(x, y, 2);
        grid[y][x] = ' ';
        grid[y][x + 1] = ' ';
    }

    grid[y][x] = cp;
    if (w > 1) grid[y][x + 1] = CELL_CONT;
    dty_mark(x, y);
    if (w > 1) dty_mark(x + 1, y);

    cell_bg(x, y, (uint32_t)w);
    if (cp == ' ') return;

    if (cp < 0x80) {                        /* ASCII 保持 8x16 点阵：等宽对齐 */
        font_draw_char(org_x + (int32_t)(x * FONT_WIDTH),
                       org_y + (int32_t)(y * FONT_HEIGHT), (char)cp, FG);
        return;
    }
    char s[5];
    cp_encode(cp, s);
    ttf_draw_text(org_x + (int32_t)(x * FONT_WIDTH),
                  org_y + (int32_t)(y * FONT_HEIGHT), s, FG, 0);
}


static void scroll_up(void) {
    for (uint32_t y = 1; y < rows; y++)
        for (uint32_t x = 0; x < cols; x++)
            grid[y - 1][x] = grid[y][x];
    for (uint32_t x = 0; x < cols; x++)
        grid[rows - 1][x] = ' ';
    dty_all();
    console_replay();
}

void console_init(void) {
    cols = 0; rows = 0; cx = 0; cy = 0;
    view_w = 0; view_h = 0;
    dty_any = 0;
    for (uint32_t y = 0; y < CONSOLE_MAX_ROWS; y++)
        for (uint32_t x = 0; x < CONSOLE_MAX_COLS; x++)
            grid[y][x] = ' ';
    blink_on = 1; cursor_drawn = 0;
}

void console_set_view(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    org_x = x; org_y = y; view_w = w; view_h = h;
    cols = w / FONT_WIDTH;
    rows = h / FONT_HEIGHT;
    if (cols > CONSOLE_MAX_COLS) cols = CONSOLE_MAX_COLS;
    if (rows > CONSOLE_MAX_ROWS) rows = CONSOLE_MAX_ROWS;
    for (uint32_t yy = 0; yy < rows; yy++)
        for (uint32_t xx = 0; xx < cols; xx++)
            grid[yy][xx] = ' ';
    cx = 0; cy = 0;
    blink_on = 1; cursor_drawn = 0;
    dty_all();
}

void console_set_origin(int32_t x, int32_t y) { org_x = x; org_y = y; }

void console_set_view_size(uint32_t w, uint32_t h) {
    view_w = w; view_h = h;
    uint32_t nc = w / FONT_WIDTH;
    uint32_t nr = h / FONT_HEIGHT;
    if (nc > CONSOLE_MAX_COLS) nc = CONSOLE_MAX_COLS;
    if (nr > CONSOLE_MAX_ROWS) nr = CONSOLE_MAX_ROWS;
    if (nc == 0 || nr == 0) return;
    cols = nc; rows = nr;
    if (cx >= cols) cx = cols - 1;
    if (cy >= rows) cy = rows - 1;
    cursor_drawn = 0;
    dty_all();
}

void console_replay(void) {
    cursor_drawn = 0;   /* 旧位置像素已被覆盖，无需恢复 */
    if (cols == 0 || rows == 0) return;
    gfx_rect(org_x, org_y, view_w, view_h, BG);   /* 先整片铺背景 */

    for (uint32_t y = 0; y < rows; y++)
        for (uint32_t x = 0; x < cols; x++) {
            uint32_t cp = grid[y][x];
            if (cp == CELL_CONT || cp == ' ') continue;
            uint32_t w = (x + 1 < cols && grid[y][x + 1] == CELL_CONT) ? 2 : 1;
            int32_t px = org_x + (int32_t)(x * FONT_WIDTH);
            int32_t py = org_y + (int32_t)(y * FONT_HEIGHT);
            if (!gfx_rect_visible(px, py, w * FONT_WIDTH, FONT_HEIGHT)) continue;
            if (cp < 0x80) {
                font_draw_char(px, py, (char)cp, FG);
                continue;
            }
            char s[5];
            cp_encode(cp, s);
            ttf_draw_text(px, py, s, FG, 0);
        }
    draw_cursor();
}

/* 桌面合成器重绘终端窗口时调用：console_replay 自己会先铺背景 */
void console_render(void) {
    if (cols == 0 || rows == 0) return;
    console_replay();
}

void console_clear(void) {
    if (cols == 0 || rows == 0) return;
    erase_cursor();
    gfx_rect(org_x, org_y, view_w, view_h, BG);
    for (uint32_t y = 0; y < rows; y++)
        for (uint32_t x = 0; x < cols; x++)
            grid[y][x] = ' ';
    cx = 0; cy = 0;
    blink_on = 1;
    dty_all();
    draw_cursor();
    if (autopresent) gfx_present();
}

void console_putc(char c) {
    if (cols == 0 || rows == 0) return;

    unsigned char u = (unsigned char)c;
    uint32_t cp = 0;
    bool emit = false;

    /* ---- UTF-8 组帧：控制台按字节收数据，多字节字先攒够再落格 ---- */
    if (u8_need) {
        if ((u & 0xC0) == 0x80) {
            u8_pend[u8_have++] = (char)u;
            if (u8_have == u8_need) {
                u8_pend[u8_have] = 0;
                const char *q = u8_pend;
                cp = utf8_decode(&q);
                u8_need = 0; u8_have = 0;
                emit = (cp != 0);
            }
        } else {
            u8_need = 0; u8_have = 0;        /* 坏序列：丢掉半个字 */
        }
    }

    if (!emit && u8_need == 0) {
        if (u < 0x80) {
            cp = (uint32_t)u;
            emit = true;
        } else if (u >= 0xC2 && u <= 0xF4) {
            u8_need = (u >= 0xF0) ? 4 : (u >= 0xE0) ? 3 : 2;
            u8_pend[0] = (char)u;
            u8_have = 1;
            return;                          /* 等续字节，不刷新光标 */
        } else {
            return;                          /* 非法首字节：丢弃 */
        }
    }
    if (!emit) return;

    erase_cursor();

    if (cp == '\n') {
        cx = 0; cy++;
    } else if (cp == '\b') {
        if (cx > 0) {
            cx--;
            if (grid[cy][cx] == CELL_CONT && cx > 0) cx--;
            clear_cell(cx, cy);
        }
    } else if (cp >= 32) {
        uint32_t w = (uint32_t)cp_width(cp);
        if (w > 1 && cx + 2 > cols) { cx = 0; cy++; }   /* 宽字不跨行 */
        plot_cp(cx, cy, cp);
        cx += w;
    }

    if (cx >= cols) { cx = 0; cy++; dty_mark(0, cy < rows ? cy : rows - 1); }
    if (cy >= rows) { scroll_up(); cy = rows - 1; }
    blink_on = 1;   /* 有输出时光标立即显示 */
    draw_cursor();
    if (put_depth == 0 && autopresent) gfx_present();
}

/* 批量输出时只在末尾 present 一次，避免逐字符刷整屏 */
void console_puts(const char *s) {
    put_depth++;
    while (*s) console_putc(*s++);
    put_depth--;
    if (put_depth == 0 && autopresent) gfx_present();
}

void console_get_cursor(uint32_t *x, uint32_t *y) {
    if (x) *x = cx;
    if (y) *y = cy;
}

void console_set_cursor(uint32_t x, uint32_t y) {
    if (cols == 0 || rows == 0) return;
    if (x >= cols) x = cols - 1;
    if (y >= rows) y = rows - 1;
    if (x != cx || y != cy) { dty_mark(cx, cy); dty_mark(x, y); }
    erase_cursor();
    cx = x; cy = y;
    blink_on = 1;
    draw_cursor();
    if (autopresent) gfx_present();
}

uint32_t console_cols(void) { return cols; }
uint32_t console_rows(void) { return rows; }

void console_write_at(uint32_t x, uint32_t y, const char *s) {
    if (cols == 0 || rows == 0) return;
    erase_cursor();
    while (*s) {
        if (x >= cols || y >= rows) break;
        unsigned char u = (unsigned char)*s++;
        if (u < 0x20) continue;              /* 输入行只接受可打印字符 */
        plot_cp(x, y, (uint32_t)u);
        x++;
    }
    blink_on = 1;   /* 不 present，由随后的 set_cursor/putc 统一刷新 */
}

/* 由 PIT 定时器周期调用，翻转光标闪烁状态；实际绘制交给桌面合成器 */
void console_blink_toggle(void) {
    blink_on = !blink_on;
}
