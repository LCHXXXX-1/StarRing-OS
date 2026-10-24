#ifndef CONSOLE_H
#define CONSOLE_H
#include <stdint.h>
#include <stdbool.h>

/* 终端内容区背景色（比窗口底色略深，形成凹陷感） */
#define CONSOLE_BG 0x000A121E

void     console_init(void);
/* 设置视图：字符网格原点（像素）与可视尺寸，并清空内容 */
void     console_set_view(int32_t origin_x, int32_t origin_y,
                          uint32_t w, uint32_t h);
/* 仅移动网格原点（窗口拖动时使用），不清空内容 */
void     console_set_origin(int32_t origin_x, int32_t origin_y);
/* 仅改变视图尺寸（窗口最大化/还原时使用），保留已有内容 */
void     console_set_view_size(uint32_t w, uint32_t h);
/* 依据字符网格重绘整个视图区（不 present，由调用方负责） */
void     console_replay(void);
/* 填充背景 + 重绘网格 + 光标；供桌面合成器在重绘终端窗口时调用 */
void     console_render(void);

void     console_putc(char c);
void     console_puts(const char *s);
void     console_clear(void);

/* 光标 */
void     console_get_cursor(uint32_t *x, uint32_t *y);
void     console_set_cursor(uint32_t x, uint32_t y);
uint32_t console_cols(void);
uint32_t console_rows(void);
void     console_blink_toggle(void);  /* 定时器调用：翻转光标闪烁状态 */

/* 在指定字符格 (x, y) 处绘制字符串（不换行、不滚动、不刷新屏幕，
   调用后需用 console_set_cursor / console_putc 触发刷新） */
void     console_write_at(uint32_t x, uint32_t y, const char *s);

/* GUI 接管显示后置 0：console 输出不再自己整屏 present，改由合成器重绘 */
void     console_set_autopresent(int on);

/* 取走"内容脏区"（像素矩形，相对屏幕），无脏区返回 false */
bool     console_take_dirty(int32_t *x, int32_t *y, uint32_t *w, uint32_t *h);
/* 当前光标所在字符格的像素矩形 */
bool     console_cursor_rect(int32_t *x, int32_t *y, uint32_t *w, uint32_t *h);
#endif
