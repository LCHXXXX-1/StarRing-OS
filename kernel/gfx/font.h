#ifndef FONT_H
#define FONT_H
#include <stdint.h>

#define FONT_WIDTH   8
#define FONT_HEIGHT  16

void font_draw_char(int32_t x, int32_t y, char c, uint32_t color);
void font_draw_str(int32_t x, int32_t y, const char *s, uint32_t color);
void font_draw_str_bg(int32_t x, int32_t y, const char *s,
                      uint32_t fg, uint32_t bg);

/* 最近邻整数倍放大；scale=1 与未缩放版本像素级一致。
   每个点阵像素画成 scale x scale 的实心方块（登录锁屏大字用 scale=4）。 */
void font_draw_char_scaled(int32_t x, int32_t y, char c,
                           uint32_t color, uint32_t scale);
void font_draw_str_scaled(int32_t x, int32_t y, const char *s,
                          uint32_t color, uint32_t scale);
#endif
