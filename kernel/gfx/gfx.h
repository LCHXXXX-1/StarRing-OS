#ifndef GFX_H
#define GFX_H
#include <stdint.h>
#include <stdbool.h>

#define GFX_MAX_WIDTH   1920
#define GFX_MAX_HEIGHT  1080

bool     gfx_init(void);
void     gfx_set_resolution(uint32_t w, uint32_t h);
uint32_t gfx_width(void);
uint32_t gfx_height(void);
void     gfx_pixel(int32_t x, int32_t y, uint32_t color);
uint32_t gfx_get_pixel(int32_t x, int32_t y);
void     gfx_fill(uint32_t color);
void     gfx_rect(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color);
/* 用预计算的「每行一个颜色」数组一次调用铺满一整片区域：内层直接写后台缓冲，
   省掉逐行 gfx_rect 的边界计算与函数调用。colors 长度不足时按最后一项钳制，
   行号越界不会写出屏幕。老机器画整幅渐变壁纸用它取代 for(y) gfx_rect(...)。 */
void     gfx_fill_row_colors(int32_t x, int32_t y, uint32_t w, uint32_t h,
                             const uint32_t *colors, uint32_t n_colors);
void     gfx_present(void);
/* 只把指定区域从后台缓冲拷到真实帧缓冲（局部合成用） */
void     gfx_present_rect(int32_t x, int32_t y, uint32_t w, uint32_t h);
/* present 末尾回调（在真实帧缓冲完成拷贝后调用），用于叠加鼠标指针等 */
void     gfx_set_present_hook(void (*fn)(void));

/* 裁剪矩形：局部合成时限定绘制范围（w==0 表示不裁剪） */
void     gfx_clip_set(int32_t x, int32_t y, uint32_t w, uint32_t h);
void     gfx_clip_reset(void);
/* 矩形是否与裁剪区、屏幕相交（用于快速跳过屏幕外的绘制） */
bool     gfx_rect_visible(int32_t x, int32_t y, uint32_t w, uint32_t h);

#endif
