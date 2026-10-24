#include "splash.h"
#include "gfx.h"
#include "font.h"
#include "font_ttf.h"
#include "boot/limine_requests.h"
#include "boot/sysinit.h"
#include "drivers/serial.h"
#include "lib/delay.h"
#include "lib/string.h"
#include "arch/x86_64/cpu.h"
#include "fontdata/logo_data.h"   /* 内核内置兜底图片 assets_logo_raw */
#include <limine.h>               /* Limine module 请求 */

/* ============================================================
 *  开机动画 = 「加载遮罩」
 *  - 优先使用 Limine 以 module 形式加载到内存的 resource/logo.raw：
 *    内核直接读取该内存地址渲染，Live CD（无硬盘）也能显示。
 *  - 没有 module 时直接用内核内置的 assets_logo_raw。
 *  - 底部跑马灯用「局部擦除 + 局部刷新」，只刷 600x6，丝滑且省带宽。
 *  - 关键改动：动画循环不再是固定 400 帧的死循环，而是
 *      每帧推进一次 system_init_step()（RTC 读取 / 渐变壁纸预计算），
 *      直到返回 true 才结束。老机器上最慢的那两件事被摊到动画里，
 *      gui_init() 只剩下拿成品数据，第一帧几乎不耗时。
 *  - 退出遮罩前清掉动画残影并直接铺上预计算好的壁纸，既不会黑屏，
 *    也不会出现长时间纯色假死。
 *  - 无动态内存、无磁盘 I/O，因此可以在 kmain 很靠前的位置调用。
 * ============================================================ */

#define LOGO_W      512u
#define LOGO_H      512u
#define LOGO_BYTES  (LOGO_W * LOGO_H * 4u)   /* 1048576 = 1MB */

_Static_assert(sizeof(assets_logo_raw) >= LOGO_BYTES,
               "built-in logo must hold a full 512x512 ARGB image");

#define SPLASH_RED   0x00FF0000u
#define SPLASH_BG    0x00000000u
#define SPLASH_CREDIT 0x00A8C4E0u   /* 品牌标语：浅蓝灰 */

/* ---- Limine module 请求 ----
   唯一实现在 boot/limine_requests.c（g_module_req），这里只 extern 使用：
   重复定义 request ID 会让 Limine 直接 PANIC。 */

/* ---------------- 进度条布局与配色（像素） ---------------- */
#define BAR_TRACK_W      600                      /* 轨道宽 */
#define BAR_TRACK_H      6                        /* 轨道高 */
#define BAR_BOTTOM_GAP   150                      /* 轨道底边距屏幕底部 150px */
#define BAR_KNOB_W       120                      /* 滑块长 */
#define BAR_TRACK_COLOR  0x00204060u              /* 深灰色轨道 */
#define BAR_KNOB_COLOR   0x00FFFFFFu              /* 纯白滑块 */
/* 循环周期：滑块从轨道左外侧整体滑入再整体滑出 = 600 + 120 = 720 */
#define BAR_SPAN         (BAR_TRACK_W + BAR_KNOB_W)
#define BAR_FRAME_DELAY  200000u                  /* 每帧 busy_wait 次数（≈60FPS，100000~500000 可调） */
/* 帧数硬上限：只是「绝不死转」的兜底。正常路径由 system_init_step()
   返回 true 退出；真走到这里说明某个步骤永远完不成，强制收尾。 */
#define SPLASH_MAX_FRAMES 20000u

/* 纯 CPU 忙等：此阶段中断尚未开启（main 的 sti 在 splash 之后），
   不能用 timer/tick 计时，因此用固定次数的自旋粗略延时。
   实现见 lib/delay.c（与磁盘工具的进度条动画共用同一份）。 */

/* 以像素为单位，把字符串水平居中绘制在给定 y 处。
   TTF 已加载（含 CJK）时按真实字宽量、走矢量渲染；否则退回 8x16 点阵。 */
static void draw_text_centered(const char *s, int32_t y, uint32_t color) {
    bool wide = false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p >= 0x80) { wide = true; break; }

    int32_t w;
    if (wide && ttf_available()) {
        w = (int32_t)ttf_get_text_width(s);
        int32_t x = ((int32_t)gfx_width() - w) / 2;
        if (x < 0) x = 0;
        ttf_draw_text(x, y, s, color, 0);
        return;
    }

    int32_t len = 0;
    while (s[len]) len++;
    w = len * FONT_WIDTH;
    int32_t x = ((int32_t)gfx_width() - w) / 2;
    if (x < 0) x = 0;
    font_draw_str(x, y, s, color);
}

/* path 的 basename 是否恰好等于 name（忽略目录部分） */
static bool basename_is(const char *path, const char *name) {
    const char *base = path;
    for (const char *q = path; q && *q; q++)
        if (*q == '/') base = q + 1;
    while (*base && *name && *base == *name) { base++; name++; }
    return *base == 0 && *name == 0;
}

/* 在 Limine 模块中查找 logo.raw，返回内存中 512x512 ARGB 像素的首地址；
   找不到或尺寸不足则返回 0。 */
static const uint32_t *logo_from_module(void) {
    if (!g_module_req.response) return 0;
    uint64_t n = g_module_req.response->module_count;
    struct limine_file **mods = g_module_req.response->modules;
    for (uint64_t i = 0; i < n; i++) {
        struct limine_file *m = mods[i];
        if (!m || !m->address || !m->path) continue;
        if (m->size < LOGO_BYTES) continue;          /* 至少要有整张图 */
        if (basename_is(m->path, "logo.raw"))
            return (const uint32_t *)m->address;
    }
    return 0;
}

/* 取 logo：Limine module 优先；没有就使用内核内置图片。
   整个路径不依赖文件系统，Ventoy/Live USB/空盘都能显示开机动画。 */
static const uint32_t *logo_pick(void) {
    const uint32_t *logo = logo_from_module();
    if (logo) {
        serial_puts("[SRCore] logo from Limine module (memory)\n");
        return logo;
    }
    serial_puts("[SRCore] logo from built-in assets_logo_raw\n");
    return (const uint32_t *)(const void *)assets_logo_raw;
}

void splash_show(void) {
    /* ---------- 0. 取得 logo 像素 ---------- */
    const uint32_t *logo = logo_pick();

    /* ---------- 1. 居中渲染 logo（ARGB：高 8 位为 alpha） ---------- */
    gfx_fill(SPLASH_BG);

    /* 小屏（1366x768 / 1024x768）上固定 512px logo + 固定 150px 底边距
       会让进度条压住 logo。这里按可用高度选择显示尺寸并保持正方形；
       1920x1080 下仍是原来的 512px。 */
    int32_t disp = (int32_t)LOGO_W;
    int32_t max_h = (int32_t)gfx_height() - 200;
    if (disp > max_h) disp = max_h;
    if (disp > (int32_t)gfx_width() - 32)
        disp = (int32_t)gfx_width() - 32;
    if (disp < 64) disp = 64;

    int32_t ox = ((int32_t)gfx_width() - disp) / 2;
    int32_t oy = ((int32_t)gfx_height() - disp) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;

    for (int32_t y = 0; y < disp; y++) {
        uint32_t sy = (uint32_t)y * LOGO_H / (uint32_t)disp;
        for (int32_t x = 0; x < disp; x++) {
            uint32_t sx = (uint32_t)x * LOGO_W / (uint32_t)disp;
            uint32_t px = logo[sy * LOGO_W + sx];
            /* 跳过完全透明的像素（alpha == 0） */
            if (((px >> 24) & 0xFFu) > 0)
                gfx_pixel(ox + x, oy + y, px & 0x00FFFFFFu);
        }
    }
    /* ---------- 1b. 品牌标语：logo 下方居中一行 ----------
       只在这里画一次，之后进度条只在自身 600x6 区域内局部刷新，
       标语不会被擦掉，动画全程可见。 */
    int32_t ty = oy + disp + 16;
    if (ty + FONT_HEIGHT > (int32_t)gfx_height())
        ty = (int32_t)gfx_height() - FONT_HEIGHT - 8;
    if (ty < 0) ty = 0;
    /* 品牌标语：开机动画跑在堆/文件系统之前，SSFN 还加载不了，
       所以字体不可用时保留 ASCII 回退，避免整行标语变空白。 */
    draw_text_centered(ttf_available() ? "由 SRCore 驱动" : "powered by SRCore",
                       ty, SPLASH_CREDIT);

    gfx_present();                       /* Logo + 标语整体只刷这一次 */

    /* ---------- 2. 进度条位置（屏幕下方居中，且不能压住标语） ---------- */
    int32_t track_x = ((int32_t)gfx_width() - BAR_TRACK_W) / 2;
    int32_t track_y = (int32_t)gfx_height() - BAR_BOTTOM_GAP;
    int32_t min_y = ty + FONT_HEIGHT + 20;
    if (track_y < min_y) track_y = min_y;
    if (track_y + BAR_TRACK_H > (int32_t)gfx_height())
        track_y = (int32_t)gfx_height() - BAR_TRACK_H;
    if (track_y < 0) track_y = 0;

    /* 状态文字行：进度条下方，只在内容变化时局部重绘 */
    int32_t label_y = track_y + BAR_TRACK_H + 10;
    bool label_ok = (label_y + (int32_t)FONT_HEIGHT <= (int32_t)gfx_height());

    /* 轨道只画一次；之后每帧只在 [track_x, track_y, 600, 6] 内做局部操作 */
    gfx_rect(track_x, track_y, BAR_TRACK_W, BAR_TRACK_H, BAR_TRACK_COLOR);
    gfx_present_rect(track_x, track_y, BAR_TRACK_W, BAR_TRACK_H);

    /* ---------- 3. 加载遮罩：跑马灯 + 每帧推进一次初始化 ---------- */
    int32_t prev_x0 = 0, prev_x1 = 0;    /* 上一帧滑块在轨道内的区间（已裁剪） */
    bool    has_prev = false;
    char    last_label[40];
    last_label[0] = 0;

    uint32_t offset = 0;
    for (; offset < SPLASH_MAX_FRAMES; offset++) {

        /* 3a. 推进一小步初始化（Step1 RTC / Step2 渐变 / Step3 收尾）。
               返回 true == 系统初始化全部完成，动画立刻结束，去登录界面。 */
        if (system_init_step()) break;

        /* 3b. 局部擦除：用轨道色覆盖「上一帧滑块」所占的区间。
               prev 区间已在上一帧裁剪到 [0, 600]，故擦除不会越出轨道。 */
        if (has_prev && prev_x1 > prev_x0)
            gfx_rect(track_x + prev_x0, track_y,
                     (uint32_t)(prev_x1 - prev_x0), BAR_TRACK_H,
                     BAR_TRACK_COLOR);

        /* 3c. 计算本帧滑块左边缘（相对轨道左边缘）：
                 x = offset % (600 + 120) - 120
               即从轨道左外侧 120px 处滑入，向右推出右边界后再次从左侧进入。 */
        int32_t x = (int32_t)(offset % BAR_SPAN) - BAR_KNOB_W;

        /* 3d. 裁剪到轨道范围 [0, 600]（绝不画到轨道外） */
        int32_t x0 = (x < 0) ? 0 : x;
        int32_t x1 = x + BAR_KNOB_W;
        if (x1 > BAR_TRACK_W) x1 = BAR_TRACK_W;

        if (x1 > x0)
            gfx_rect(track_x + x0, track_y,
                     (uint32_t)(x1 - x0), BAR_TRACK_H, BAR_KNOB_COLOR);

        prev_x0 = x0;
        prev_x1 = x1;
        has_prev = true;

        /* 3e. 状态文字：仅在内容变化时重绘那一条 600x16 的带，
               不污染 logo，也不会每帧多写一遍 MMIO 帧缓冲。 */
        if (label_ok) {
            const char *lb = system_init_label();
            if (kstrcmp(lb, last_label) != 0) {
                gfx_rect(track_x, label_y, BAR_TRACK_W, FONT_HEIGHT, SPLASH_BG);
                draw_text_centered(lb, label_y, SPLASH_CREDIT);
                kstrcpy(last_label, lb);
                gfx_present_rect(track_x, label_y, BAR_TRACK_W, FONT_HEIGHT);
            }
        }

        /* 3f. 局部刷新：只把 600x6 的进度条区域合成到真实帧缓冲，
               绝不用全屏 gfx_present()，这是丝滑的关键。 */
        gfx_present_rect(track_x, track_y, BAR_TRACK_W, BAR_TRACK_H);

        /* 3g. 帧率控制（约 60FPS；数值 100000~500000 之间可按机器调） */
        busy_wait(BAR_FRAME_DELAY);
    }

    if (offset >= SPLASH_MAX_FRAMES) {
        serial_puts("[SRCore] splash: frame budget hit, force init finish\n");
        system_init_force_done();          /* 兜底：绝不允许无限转圈 */
    }

    /* ---------- 4. 退出遮罩：清屏 + 铺上预计算好的壁纸 ----------
       用 bg_gradient_cache 一次调用铺满整屏（不再逐行 mix），
       所以动画结束后既没有黑屏也没有红屏空窗期，桌面第一帧只是
       在同一张图上叠加图标/任务栏。 */
    gfx_fill(SPLASH_BG);
    {
        uint32_t rows = bg_cache_rows;
        if (rows)
            gfx_fill_row_colors(0, 0, gfx_width(), gfx_height(),
                                bg_gradient_cache, rows);
    }
    gfx_present();
    serial_puts("[SRCore] done\n");
}
