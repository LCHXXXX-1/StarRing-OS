#include "drivers/framebuffer.h"

static struct limine_framebuffer *fb = 0;

bool fb_init(struct limine_framebuffer_response *resp) {
    if (!resp || resp->framebuffer_count < 1) return false;

    struct limine_framebuffer *f = resp->framebuffers[0];
    if (!f || !f->address || f->width == 0 || f->height == 0) return false;

    /* Limine 的 VBE 回退只用 32/24/16bpp；8bpp 调色板模式没有可用的
       调色板信息，直接拒绝比画出一屏乱码更好。 */
    if (f->bpp != 32 && f->bpp != 24 && f->bpp != 16 && f->bpp != 15)
        return false;

    /* 真机自检：地址与几何必须自洽，否则任何像素写入都会写飞
       （越界写 MMIO 空洞常见后果就是三重故障/整机卡死）。
         - 行宽必须至少容纳 width 个像素，且不能是离谱值；
         - 分辨率必须在合理范围；
         - 整块区域大小不能算出溢出。 */
    uint64_t bytes_pp = (uint64_t)((f->bpp + 7u) / 8u);
    uint64_t pitch     = (uint64_t)f->pitch;
    uint64_t need      = (uint64_t)f->width * bytes_pp;

    if (f->width  < 320u   || f->width  > 16384u) return false;
    if (f->height < 200u   || f->height > 16384u) return false;
    if (pitch < need || pitch > (1ull << 20)) return false;
    if ((uint64_t)f->address < 0x1000ull)     return false;

    fb = f;
    return true;
}

struct limine_framebuffer *fb_get(void) { return fb; }
uint64_t fb_width(void)  { return fb ? fb->width  : 0; }
uint64_t fb_height(void) { return fb ? fb->height : 0; }
uint32_t fb_pitch(void)  { return fb ? (uint32_t)fb->pitch : 0; }
uint16_t fb_bpp(void)    { return fb ? fb->bpp : 0; }

/* 把 8 位颜色通道放进帧缓冲的掩码位置。mask_size 为 5/6/8 时都按
   “取高 mask_size 位”缩放，符合 VBE 的 16bpp 5-6-5 / 24bpp 8-8-8。 */
static inline uint32_t pack_channel(uint32_t c, uint8_t size, uint8_t shift) {
    if (size == 0) return 0;
    if (size > 8) size = 8;
    uint32_t v = (c >> (8 - size)) & ((1u << size) - 1u);
    return v << shift;
}

uint32_t fb_pack(uint32_t rgb) {
    if (!fb) return rgb;
    if (fb->memory_model != LIMINE_FRAMEBUFFER_RGB) return rgb;

    uint32_t r = (rgb >> 16) & 0xFFu;
    uint32_t g = (rgb >> 8)  & 0xFFu;
    uint32_t b =  rgb        & 0xFFu;
    return pack_channel(r, fb->red_mask_size,   fb->red_mask_shift)   |
           pack_channel(g, fb->green_mask_size, fb->green_mask_shift) |
           pack_channel(b, fb->blue_mask_size,  fb->blue_mask_shift);
}

void fb_putpixel(int32_t x, int32_t y, uint32_t rgb) {
    if (!fb || !fb->address) return;
    if (x < 0 || y < 0 ||
        (uint64_t)x >= fb->width || (uint64_t)y >= fb->height) return;

    uint8_t *row = (uint8_t *)fb->address + (uint64_t)y * fb->pitch;
    uint32_t v = fb_pack(rgb);

    switch (fb->bpp) {
    case 32:
        *(uint32_t *)(row + (uint64_t)x * 4u) = v;
        break;
    case 24:
        row += (uint64_t)x * 3u;
        row[0] = (uint8_t)(v);
        row[1] = (uint8_t)(v >> 8);
        row[2] = (uint8_t)(v >> 16);
        break;
    case 16:
    case 15:
        *(uint16_t *)(row + (uint64_t)x * 2u) = (uint16_t)v;
        break;
    default:
        break;
    }
}

static inline uint32_t unpack_channel(uint32_t v, uint8_t size, uint8_t shift) {
    if (size == 0) return 0;
    if (size > 8) size = 8;
    uint32_t c = (v >> shift) & ((1u << size) - 1u);
    return (c * 255u) / ((1u << size) - 1u);
}

uint32_t fb_getpixel(int32_t x, int32_t y) {
    if (!fb || !fb->address) return 0;
    if (x < 0 || y < 0 ||
        (uint64_t)x >= fb->width || (uint64_t)y >= fb->height) return 0;

    const uint8_t *row = (const uint8_t *)fb->address + (uint64_t)y * fb->pitch;
    uint32_t v = 0;

    switch (fb->bpp) {
    case 32:
        v = *(const uint32_t *)(row + (uint64_t)x * 4u);
        break;
    case 24:
        row += (uint64_t)x * 3u;
        v = (uint32_t)row[0] | ((uint32_t)row[1] << 8) | ((uint32_t)row[2] << 16);
        break;
    case 16:
    case 15:
        v = *(const uint16_t *)(row + (uint64_t)x * 2u);
        break;
    default:
        return 0;
    }

    return (unpack_channel(v, fb->red_mask_size,   fb->red_mask_shift)   << 16) |
           (unpack_channel(v, fb->green_mask_size, fb->green_mask_shift) << 8)  |
            unpack_channel(v, fb->blue_mask_size,  fb->blue_mask_shift);
}
