#include "font.h"
#include "gfx.h"
#include "gui.h"
#include "fontdata/font8x16.h"

void font_draw_char(int32_t x, int32_t y, char c, uint32_t color) {
    uint8_t idx = (uint8_t)c;
    if (idx < 32 || idx > 126) return;
    if (!gfx_rect_visible(x, y, FONT_WIDTH, FONT_HEIGHT)) return;
    idx -= 32;
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = font8x16[idx][row];
        if (!bits) continue;
        for (int col = 0; col < FONT_WIDTH; col++)
            if (bits & (0x80 >> col))
                gfx_pixel(x + col, y + row, color);
    }
}
void font_draw_str(int32_t x, int32_t y, const char *s, uint32_t color) {
    while (*s) { font_draw_char(x, y, *s, color); x += FONT_WIDTH; s++; }
}
void font_draw_str_bg(int32_t x, int32_t y, const char *s,
                      uint32_t fg, uint32_t bg) {
    while (*s) {
        gfx_rect(x, y, FONT_WIDTH, FONT_HEIGHT, bg);
        font_draw_char(x, y, *s, fg);
        x += FONT_WIDTH; s++;
    }
}

void font_draw_char_scaled(int32_t x, int32_t y, char c,
                           uint32_t color, uint32_t scale) {
    if (scale == 0) return;
    if (scale == 1) { font_draw_char(x, y, c, color); return; }

    uint8_t idx = (uint8_t)c;
    if (idx < 32 || idx > 126) return;

    /* 先用 32 位乘避免 8*scale/16*scale 在极端 scale 下溢出 int32 坐标。 */
    int64_t bw = (int64_t)FONT_WIDTH  * (int64_t)scale;
    int64_t bh = (int64_t)FONT_HEIGHT * (int64_t)scale;
    if (bw > 0x7FFFFFFF || bh > 0x7FFFFFFF) return;

    int32_t w = (int32_t)bw, h = (int32_t)bh;
    if (!gfx_rect_visible(x, y, (uint32_t)w, (uint32_t)h)) return;

    idx -= 32;
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = font8x16[idx][row];
        if (!bits) continue;
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (!(bits & (0x80 >> col))) continue;
            gui_fill(x + col * (int32_t)scale,
                     y + row * (int32_t)scale,
                     scale, scale, color);
        }
    }
}

void font_draw_str_scaled(int32_t x, int32_t y, const char *s,
                          uint32_t color, uint32_t scale) {
    if (scale == 0) return;
    while (*s) {
        font_draw_char_scaled(x, y, *s, color, scale);
        x += (int32_t)((uint32_t)FONT_WIDTH * scale);
        s++;
    }
}
