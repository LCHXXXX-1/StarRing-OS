#include "gfx.h"
#include "drivers/framebuffer.h"
#include "lib/mem.h"          /* memcpy：1:1 present 整行拷贝 */

/* ---- 临时排查（真机卡死）：把内核虚拟屏强制限制为 1024x768 ----
   真机 1440p/4K 下整屏合成一次要写几十 MB，先排除分辨率/显存带宽因素。
   实际帧缓冲尺寸不变，缩放在 present_scaled() 里完成。
   排查结束后把 GFX_FORCE_W/H 改回 GFX_MAX_WIDTH/HEIGHT 即可。 */
#define GFX_FORCE_W  1024u
#define GFX_FORCE_H  768u

static uint32_t backbuffer[GFX_MAX_WIDTH * GFX_MAX_HEIGHT];
static uint32_t cur_w = 0, cur_h = 0;
static volatile int in_present = 0;

/* 裁剪矩形；clip_w == 0 表示不裁剪 */
static int32_t  clip_x = 0, clip_y = 0;
static uint32_t clip_w = 0, clip_h = 0;

static void (*present_hook_fn)(void) = 0;
void gfx_set_present_hook(void (*fn)(void)) { present_hook_fn = fn; }

void gfx_clip_reset(void) { clip_w = 0; clip_h = 0; }
void gfx_clip_set(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    clip_x = x; clip_y = y; clip_w = w; clip_h = h;
}

static inline bool clip_ok(int32_t x, int32_t y) {
    if (clip_w == 0) return true;
    return (uint32_t)(x - clip_x) < clip_w && (uint32_t)(y - clip_y) < clip_h;
}

bool gfx_rect_visible(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return false;
    int32_t x0 = x, y0 = y, x1 = x + (int32_t)w, y1 = y + (int32_t)h;
    if (clip_w) {
        if (x0 < clip_x) x0 = clip_x;
        if (y0 < clip_y) y0 = clip_y;
        if (x1 > clip_x + (int32_t)clip_w) x1 = clip_x + (int32_t)clip_w;
        if (y1 > clip_y + (int32_t)clip_h) y1 = clip_y + (int32_t)clip_h;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int32_t)cur_w) x1 = (int32_t)cur_w;
    if (y1 > (int32_t)cur_h) y1 = (int32_t)cur_h;
    return x0 < x1 && y0 < y1;
}

bool gfx_init(void) {
    if (!fb_get()) return false;
    uint32_t w = (uint32_t)fb_width(), h = (uint32_t)fb_height();
    if (w > GFX_FORCE_W)    w = GFX_FORCE_W;
    if (h > GFX_FORCE_H)    h = GFX_FORCE_H;
    if (w > GFX_MAX_WIDTH)  w = GFX_MAX_WIDTH;
    if (h > GFX_MAX_HEIGHT) h = GFX_MAX_HEIGHT;
    gfx_set_resolution(w, h);
    return true;
}
void gfx_set_resolution(uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return;
    if (w > GFX_FORCE_W)    w = GFX_FORCE_W;
    if (h > GFX_FORCE_H)    h = GFX_FORCE_H;
    if (w > GFX_MAX_WIDTH)  w = GFX_MAX_WIDTH;
    if (h > GFX_MAX_HEIGHT) h = GFX_MAX_HEIGHT;
    cur_w = w; cur_h = h;
    gfx_clip_reset();
    gfx_fill(0x000000);
}
uint32_t gfx_width(void)  { return cur_w; }
uint32_t gfx_height(void) { return cur_h; }

void gfx_pixel(int32_t x, int32_t y, uint32_t color) {
    if (x < 0 || y < 0) return;
    if ((uint32_t)x >= cur_w || (uint32_t)y >= cur_h) return;
    if (!clip_ok(x, y)) return;
    backbuffer[(uint32_t)y * cur_w + (uint32_t)x] = color;
}

uint32_t gfx_get_pixel(int32_t x, int32_t y) {
    if (x < 0 || y < 0) return 0;
    if ((uint32_t)x >= cur_w || (uint32_t)y >= cur_h) return 0;
    return backbuffer[(uint32_t)y * cur_w + (uint32_t)x];
}

void gfx_fill(uint32_t color) {
    uint32_t n = cur_w * cur_h;
    for (uint32_t i = 0; i < n; i++) backbuffer[i] = color;
}

/* 逐行填充：裁剪只做一次，内层是紧凑的 32 位写循环（比逐像素快得多） */
void gfx_rect(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (w == 0 || h == 0) return;
    int32_t x0 = x, y0 = y;
    int32_t x1 = x + (int32_t)w, y1 = y + (int32_t)h;
    if (clip_w) {
        if (x0 < clip_x) x0 = clip_x;
        if (y0 < clip_y) y0 = clip_y;
        if (x1 > clip_x + (int32_t)clip_w) x1 = clip_x + (int32_t)clip_w;
        if (y1 > clip_y + (int32_t)clip_h) y1 = clip_y + (int32_t)clip_h;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int32_t)cur_w) x1 = (int32_t)cur_w;
    if (y1 > (int32_t)cur_h) y1 = (int32_t)cur_h;
    if (x0 >= x1 || y0 >= y1) return;

    uint32_t n = (uint32_t)(x1 - x0);
    for (int32_t yy = y0; yy < y1; yy++) {
        uint32_t *dst = &backbuffer[(uint32_t)yy * cur_w + (uint32_t)x0];
        for (uint32_t i = 0; i < n; i++) dst[i] = color;
    }
}

/* 用预计算的「每行一个颜色」数组铺满一整片区域。
   行号用**屏幕绝对坐标 y** 索引 colors，越界按最后一项钳制（绝不越界读）。
   整幅渐变壁纸只需一次调用 + 紧凑的标量写循环，绕开逐行 gfx_rect：
   老机器上省下 sh 次边界计算/裁剪/函数调用，也省下 mix() 的除法。 */
void gfx_fill_row_colors(int32_t x, int32_t y, uint32_t w, uint32_t h,
                         const uint32_t *colors, uint32_t n_colors) {
    if (!colors || n_colors == 0 || w == 0 || h == 0) return;

    int32_t x0 = x, y0 = y;
    int32_t x1 = x + (int32_t)w, y1 = y + (int32_t)h;
    if (clip_w) {
        if (x0 < clip_x) x0 = clip_x;
        if (y0 < clip_y) y0 = clip_y;
        if (x1 > clip_x + (int32_t)clip_w) x1 = clip_x + (int32_t)clip_w;
        if (y1 > clip_y + (int32_t)clip_h) y1 = clip_y + (int32_t)clip_h;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int32_t)cur_w) x1 = (int32_t)cur_w;
    if (y1 > (int32_t)cur_h) y1 = (int32_t)cur_h;
    if (x0 >= x1 || y0 >= y1) return;

    uint32_t n    = (uint32_t)(x1 - x0);
    uint32_t last = n_colors - 1u;

    for (int32_t yy = y0; yy < y1; yy++) {
        uint32_t ri = (uint32_t)yy;
        if (ri > last) ri = last;
        uint32_t c  = colors[ri];
        uint32_t *dst = &backbuffer[(uint32_t)yy * cur_w + (uint32_t)x0];

        uint32_t i = 0;                 /* 手动 4x 展开：-mno-sse 下标量写最快 */
        for (; i + 3u < n; i += 4u) {
            dst[i] = c; dst[i + 1] = c; dst[i + 2] = c; dst[i + 3] = c;
        }
        for (; i < n; i++) dst[i] = c;
    }
}

/* 32bpp 时的通道布局：1 = RRGGBB，2 = BBGGRR，0 = 其它掩码（走 fb_pack） */
static int fb_layout32(const struct limine_framebuffer *fb) {
    if (fb->red_mask_size == 8 && fb->green_mask_size == 8 &&
        fb->blue_mask_size == 8 && fb->green_mask_shift == 8) {
        if (fb->red_mask_shift == 16 && fb->blue_mask_shift == 0) return 1;
        if (fb->red_mask_shift == 0  && fb->blue_mask_shift == 16) return 2;
    }
    return 0;
}

/* 把 0x00RRGGBB 写进真实帧缓冲的某一行第 dx 个像素（行指针由调用方算好，
   避免每像素重新计算地址和做掩码转换）。 */
static inline void fb_put_rowpixel(const struct limine_framebuffer *fb,
                                   uint8_t *drow, int32_t dx, uint32_t c,
                                   int layout) {
    if (fb->bpp == 32) {
        uint32_t *p = (uint32_t *)drow + dx;
        if (layout == 1)      *p = c;
        else if (layout == 2) *p = ((c & 0x000000FFu) << 16) |
                                   (c & 0x0000FF00u) |
                                   ((c >> 16) & 0x000000FFu);
        else                  *p = fb_pack(c);
    } else if (fb->bpp == 24) {
        uint32_t v = fb_pack(c);
        uint8_t *p = drow + (uint64_t)dx * 3u;
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16);
    } else {                                   /* 16bpp / 15bpp */
        ((uint16_t *)drow)[dx] = (uint16_t)fb_pack(c);
    }
}

/* 1:1 拷贝。真机常见路径；32bpp 时按通道布局直接写，避免逐像素调用。 */
static void present_1to1(struct limine_framebuffer *fb,
                         int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    uint32_t n = (uint32_t)(x1 - x0);
    int layout = (fb->bpp == 32) ? fb_layout32(fb) : 0;

    if (fb->bpp == 32 && layout == 1) {
        /* 掩码与后台缓冲完全一致：整行 memcpy，老机器上最快的一条路径 */
        for (int32_t yy = y0; yy < y1; yy++) {
            const uint32_t *src =
                &backbuffer[(uint32_t)yy * cur_w + (uint32_t)x0];
            uint32_t *dst = (uint32_t *)((uint8_t *)fb->address +
                                         (uint64_t)yy * fb->pitch) + x0;
            memcpy(dst, src, (size_t)n * sizeof(uint32_t));
        }
        return;
    }

    if (fb->bpp == 32) {
        for (int32_t yy = y0; yy < y1; yy++) {
            const uint32_t *src =
                &backbuffer[(uint32_t)yy * cur_w + (uint32_t)x0];
            uint8_t *drow = (uint8_t *)fb->address + (uint64_t)yy * fb->pitch;
            for (uint32_t i = 0; i < n; i++)
                fb_put_rowpixel(fb, drow, x0 + (int32_t)i, src[i], layout);
        }
        return;
    }

    if (fb->bpp == 24) {
        for (int32_t yy = y0; yy < y1; yy++) {
            const uint32_t *src =
                &backbuffer[(uint32_t)yy * cur_w + (uint32_t)x0];
            uint8_t *drow = (uint8_t *)fb->address + (uint64_t)yy * fb->pitch;
            for (uint32_t i = 0; i < n; i++)
                fb_put_rowpixel(fb, drow, x0 + (int32_t)i, src[i], 0);
        }
        return;
    }

    /* 16bpp / 15bpp */
    for (int32_t yy = y0; yy < y1; yy++) {
        const uint32_t *src =
            &backbuffer[(uint32_t)yy * cur_w + (uint32_t)x0];
        uint16_t *dst = (uint16_t *)((uint8_t *)fb->address +
                                     (uint64_t)yy * fb->pitch) + x0;
        for (uint32_t i = 0; i < n; i++)
            dst[i] = (uint16_t)fb_pack(src[i]);
    }
}

/* 虚拟分辨率与真实帧缓冲不一致时，把虚拟屏的一个矩形缩放到物理屏。
   旧实现每像素做两次 64 位除法，真机全屏刷新慢到像“死机”：
     - 行：sy 每行只算一次（原来也是每行一次，但源行指针没提出来）；
     - 列：sx 用“余数累加”推进，等价于 sx = dx*cur_w/sw 却完全不做除法；
     - 写像素：直接按行指针 + 通道布局写，不再调 fb_putpixel。 */
static void present_scaled(struct limine_framebuffer *fb,
                           int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    uint32_t sw = (uint32_t)fb->width;
    uint32_t sh = (uint32_t)fb->height;
    if (!sw || !sh || !cur_w || !cur_h) return;

    int32_t dx0 = (int32_t)((uint64_t)(uint32_t)x0 * sw / cur_w);
    int32_t dx1 = (int32_t)(((uint64_t)(uint32_t)x1 * sw + cur_w - 1u) / cur_w);
    int32_t dy0 = (int32_t)((uint64_t)(uint32_t)y0 * sh / cur_h);
    int32_t dy1 = (int32_t)(((uint64_t)(uint32_t)y1 * sh + cur_h - 1u) / cur_h);
    if (dx1 > (int32_t)sw) dx1 = (int32_t)sw;
    if (dy1 > (int32_t)sh) dy1 = (int32_t)sh;
    if (dx0 < 0) dx0 = 0;
    if (dy0 < 0) dy0 = 0;

    int layout = (fb->bpp == 32) ? fb_layout32(fb) : 0;

    for (int32_t dy = dy0; dy < dy1; dy++) {
        uint32_t sy = (uint32_t)((uint64_t)(uint32_t)dy * cur_h / sh);
        if (sy >= cur_h) sy = cur_h - 1;
        const uint32_t *srcrow = &backbuffer[(uint64_t)sy * cur_w];
        uint8_t *drow = (uint8_t *)fb->address + (uint64_t)dy * fb->pitch;

        /* num = dx*cur_w 的增量形式：sx = num/sw，rem = num%sx*sw 的余数。
           每列只做一次加法 + 至多一次减法，结果与逐列除法完全一致。 */
        uint32_t num0 = (uint32_t)dx0 * cur_w;
        uint32_t sx   = num0 / sw;
        uint32_t rem  = num0 - sx * sw;

        for (int32_t dx = dx0; dx < dx1; dx++) {
            uint32_t s = (sx >= cur_w) ? cur_w - 1 : sx;
            fb_put_rowpixel(fb, drow, dx, srcrow[s], layout);
            rem += cur_w;
            while (rem >= sw) { rem -= sw; sx++; }
        }
    }
}

void gfx_present_rect(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    /* 不可重入：鼠标中断可能在拷贝中途触发一次合成，
       此时直接返回，由外层这次 present 统一拷贝并叠加指针 */
    if (in_present) return;
    in_present = 1;

    struct limine_framebuffer *fb = fb_get();
    if (fb && cur_w != 0 && cur_h != 0) {
        int32_t x0 = x, y0 = y;
        int32_t x1 = x + (int32_t)w, y1 = y + (int32_t)h;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int32_t)cur_w) x1 = (int32_t)cur_w;
        if (y1 > (int32_t)cur_h) y1 = (int32_t)cur_h;

        if (x0 < x1 && y0 < y1) {
            uint64_t sw = fb->width, sh = fb->height;
            if (sw == cur_w && sh == cur_h)
                present_1to1(fb, x0, y0, x1, y1);
            else
                present_scaled(fb, x0, y0, x1, y1);
        }
    }

    in_present = 0;
    if (present_hook_fn) present_hook_fn();
}

void gfx_present(void) { gfx_present_rect(0, 0, cur_w, cur_h); }
