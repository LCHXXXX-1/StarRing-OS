#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H
#include <stdint.h>
#include <stdbool.h>
#include <limine.h>

bool fb_init(struct limine_framebuffer_response *resp);
struct limine_framebuffer *fb_get(void);
uint64_t fb_width(void);
uint64_t fb_height(void);
uint32_t fb_pitch(void);
uint16_t fb_bpp(void);

/* 以 0x00RRGGBB 读写真实帧缓冲。
   真机 BIOS/VBE 不一定给 32bpp：常见还有 24bpp 和 16bpp，而且通道
   顺序由 red/green/blue_mask_shift 决定。旧代码固定按 32bpp 写，遇到
   24bpp 会把画面写成斜条纹/花屏，遇到 16bpp 则完全错位。 */
void     fb_putpixel(int32_t x, int32_t y, uint32_t rgb);
uint32_t fb_getpixel(int32_t x, int32_t y);
/* 0x00RRGGBB -> 帧缓冲原生像素值 */
uint32_t fb_pack(uint32_t rgb);
#endif
