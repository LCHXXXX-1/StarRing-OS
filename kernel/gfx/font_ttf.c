/* ================================================================
 *  font_ttf.c — SSFN 矢量字体引擎集成
 *
 *  本文件同时充当 SSFN 的实现编译单元（#define SSFN_IMPLEMENTATION）。
 *  提供 realloc 垫片让 SSFN 使用内核堆。
 * ================================================================ */

#include "mm/heap.h"
#include "lib/mem.h"
#include "lib/string.h"

/* ---------- SSFN 内存垫片 ---------- */

static void *ssfn_realloc_shim(void *ptr, size_t sz) {
    if (!ptr) return _kmalloc((uint64_t)sz, "ssfn", 0);
    if (!sz) { kfree(ptr); return 0; }

    /* realloc 语义只保证旧数据的前 min(旧容量, 新容量) 字节有效。
       旧实现按 sz（新长度）从旧指针拷贝，会读过旧块末尾——堆尾紧邻
       未映射页时会直接 #PF(read)。必须按真实容量截断。 */
    uint64_t old = kheap_usable_size(ptr);
    void *np = _kmalloc((uint64_t)sz, "ssfn", 0);
    if (!np) return 0;
    if (old > (uint64_t)sz) old = (uint64_t)sz;
    if (old) memcpy(np, ptr, (size_t)old);
    kfree(ptr);
    return np;
}
static void ssfn_free_shim(void *p) { kfree(p); }

#define SSFN_realloc  ssfn_realloc_shim
#define SSFN_free     ssfn_free_shim
#define SSFN_memset   memset
#define SSFN_memcpy   memcpy

static int ssfn_memcmp_shim(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}
#define SSFN_memcmp   ssfn_memcmp_shim

#define SSFN_IMPLEMENTATION
#include "fontdata/ssfn.h"

/* ---------- 模块内部状态 ---------- */

#include "font_ttf.h"
#include "font.h"
#include "gfx.h"
#include "boot/limine_requests.h"
#include "drivers/framebuffer.h"
#include "fs/fs.h"
#include "fs/srs.h"
#include "drivers/serial.h"

#define TTF_FONT_SIZE   16
#define TTF_RENDER_W    128
#define TTF_RENDER_H    128
#define CACHE_SLOTS     512

static ssfn_t          ctx;
static uint8_t        *font_blob;
static bool            loaded;
static bool            blob_owned;   /* true: font_blob 是 kmalloc 的，需释放 */

/* 实测行高：ssfn_select() 之后 ctx.f 仍是 NULL、ctx.line 仍是 0，
   任何地方都不能拿它们算坐标/偏移；这里用真实字形量一次并存下来。 */
static uint32_t        line_px = TTF_FONT_SIZE;

/* 单字形渲染缓冲（ARGB） */
static uint8_t         render_mem[TTF_RENDER_W * TTF_RENDER_H * 4];

/* ---------- 字形缓存 ---------- */

typedef struct {
    uint32_t cp;
    uint8_t  w, h;       /* 字模单元：宽 = 左 bearing + 步进，高 = 行高 */
    uint8_t  advance;    /* 步进宽（像素） */
    uint8_t *gray;       /* w*h 灰度 (kmalloc)，行跨度 = w */
    uint32_t stamp;      /* LRU 时间戳 */
} glyph_t;

static glyph_t  cache[CACHE_SLOTS];
static uint32_t clock_tick;

/* 缺字/渲染失败诊断计数：只用来在串口留证，不参与渲染决策 */
static uint32_t missing_logged;
static uint32_t render_fail_logged;

static inline uint32_t cp_hash(uint32_t cp) {
    return (cp * 2654435761u) >> 23;
}

static glyph_t *cache_lookup(uint32_t cp) {
    uint32_t idx = cp_hash(cp);
    for (uint32_t i = 0; i < 4; i++) {
        glyph_t *g = &cache[(idx + i) & (CACHE_SLOTS - 1)];
        if (g->cp == cp && g->gray) { g->stamp = ++clock_tick; return g; }
    }
    return 0;
}

static glyph_t *cache_evict(uint32_t cp) {
    uint32_t idx = cp_hash(cp);
    glyph_t *slot = &cache[idx & (CACHE_SLOTS - 1)];
    for (uint32_t i = 1; i < 4; i++) {
        glyph_t *g = &cache[(idx + i) & (CACHE_SLOTS - 1)];
        if (!g->cp || !g->gray) { slot = g; break; }
    }
    if (slot->gray) { kfree(slot->gray); slot->gray = 0; }
    slot->cp = cp;
    slot->stamp = ++clock_tick;
    return slot;
}

void ttf_cache_clear(void) {
    for (uint32_t i = 0; i < CACHE_SLOTS; i++) {
        if (cache[i].gray) kfree(cache[i].gray);
        cache[i].gray = 0; cache[i].cp = 0;
    }
    clock_tick = 0;
}

/* ---------- 渲染单字形 ---------- */

/* 单字形渲染结果：ttf_draw_text 据此决定画哪个颜色的诊断方块 */
enum {
    GLYPH_OK = 0,      /* 有字模、有墨水 */
    GLYPH_EMPTY,       /* 码点在字体表里，但 bbox/轮廓为空 */
    GLYPH_FAIL         /* ssfn_render 失败 / 字模超界 / 缓存分配失败 */
};

static int rasterize(uint32_t cp, glyph_t *out) {
    if (!out) return GLYPH_FAIL;
    out->gray = 0; out->w = 0; out->h = 0; out->advance = 0;
    if (!loaded) return GLYPH_FAIL;

    char utf8[5]; int len = 0;
    if (cp < 0x80) { utf8[0] = (char)cp; len = 1; }
    else if (cp < 0x800) {
        utf8[0] = (char)(0xC0|cp>>6); utf8[1] = (char)(0x80|(cp&0x3F)); len = 2;
    } else if (cp < 0x10000) {
        utf8[0] = (char)(0xE0|cp>>12);
        utf8[1] = (char)(0x80|((cp>>6)&0x3F));
        utf8[2] = (char)(0x80|(cp&0x3F)); len = 3;
    } else {
        utf8[0] = (char)(0xF0|cp>>18);
        utf8[1] = (char)(0x80|((cp>>12)&0x3F));
        utf8[2] = (char)(0x80|((cp>>6)&0x3F));
        utf8[3] = (char)(0x80|(cp&0x3F)); len = 4;
    }
    utf8[len] = 0;

    int w, h, left, top;
    if (ssfn_bbox(&ctx, utf8, &w, &h, &left, &top) != SSFN_OK || w <= 0 || h <= 0) {
        /* 缺字：把码点打出来。VMware 上如果这里连刷 8 条 CJK，
           说明字体文件没加载完整（或根本没有 CJK 段），不是布局问题。 */
        if (cp >= 0x80 && missing_logged < 8) {
            missing_logged++;
            serial_puts("[SRCore] font: NO GLYPH U+");
            serial_puthex(cp);
            serial_puts("\n");
        }
        /* 空白 / 缺字：没有可画像素，但必须留下非零步进，
           否则 ttf_draw_text() 的推进循环会停在这一格 */
        out->advance = (uint8_t)(w > 0 ? w : TTF_FONT_SIZE / 2);
        return GLYPH_EMPTY;
    }
    if (w > TTF_RENDER_W || h > TTF_RENDER_H) return GLYPH_FAIL;

    ssfn_buf_t dst;
    dst.ptr = render_mem;
    dst.w = w; dst.h = h; dst.p = (uint16_t)(w * 4);
    /* SSFN 的 (x,y) 是画笔原点（基线起点），不是画布左上角：
       单字形必须从 bbox 给出的 (left, top) 起笔。从 (0,0) 起笔会让
       整个字模向左上偏移，顶部 top 行被裁掉——中文只剩底部几条像素。 */
    dst.x = left; dst.y = top;
    dst.fg = 0xFFFFFFFF;
    dst.bg = 0;
    memset(render_mem, 0, (size_t)(w * h * 4));

    int rr = ssfn_render(&ctx, &dst, utf8);
    if (rr <= 0) {
        if (render_fail_logged < 4) {
            render_fail_logged++;
            serial_puts("[SRCore] font: ssfn_render rc=");
            serial_putdec((uint64_t)(rr < 0 ? -rr : 0));
            serial_puts(" U+");
            serial_puthex(cp);
            serial_puts(" heap_free=");
            serial_putdec(heap_free());
            serial_puts("\n");
        }
        out->gray = 0;
        return GLYPH_FAIL;
    }
    /* 渲染后必须真的有可用的字形数据，否则宁可不画也不能解引用野指针 */
    if (!ctx.g || ctx.g->p <= 0 || ctx.g->h <= 0) { out->gray = 0; return GLYPH_FAIL; }

    uint8_t *gray = (uint8_t *)_kmalloc((uint64_t)(w * h), "font", __LINE__);
    if (!gray) {
        /* VMware 内存/碎片导致的典型症状：字体本体在，逐字缓存分配不出来 */
        if (render_fail_logged < 4) {
            render_fail_logged++;
            serial_puts("[SRCore] font: glyph alloc failed, heap_free=");
            serial_putdec(heap_free());
            serial_puts("\n");
        }
        out->gray = 0;
        return GLYPH_FAIL;
    }
    uint32_t *px = (uint32_t *)render_mem;
    for (int i = 0; i < w * h; i++)
        gray[i] = (uint8_t)(px[i] >> 24);

    int adv = dst.x - left;               /* 渲染后画笔前进了多少 */
    if (adv <= 0) adv = w;                /* 防御：0 步进会让绘制循环原地打转 */
    if (adv > 255) adv = 255;

    out->w = (uint8_t)w;
    out->h = (uint8_t)h;
    out->advance = (uint8_t)adv;
    out->gray = gray;
    return GLYPH_OK;
}

/* ---------- Alpha 混合 ---------- */

static inline uint32_t blend(uint32_t bg, uint32_t fg, uint8_t a) {
    if (a == 0)   return bg;
    if (a == 255) return fg;
    uint32_t inv = 255u - a;
    uint32_t r = (((bg>>16)&0xFF)*inv + ((fg>>16)&0xFF)*a) >> 8;
    uint32_t g = (((bg>>8)&0xFF)*inv  + ((fg>>8)&0xFF)*a)  >> 8;
    uint32_t b = (( bg&0xFF)*inv      + ( fg&0xFF)*a)       >> 8;
    return (r << 16) | (g << 8) | b;
}

/* ---------- 公共 API ---------- */

uint32_t utf8_decode(const char **s) {
    char *p = (char *)*s;
    uint32_t cp = ssfn_utf8(&p);
    *s = (const char *)p;
    return cp;
}

/* 字体文件指纹：QEMU / VMware 各跑一次，把两边的 size + hash 对一下，
   立刻能看出是不是盘上的 siyuan.sfn 不是同一份 / 读进来被截断。 */
static uint32_t font_hash(const uint8_t *p, uint64_t n) {
    uint32_t h = 2166136261u;
    if (!p) return 0;
    for (uint64_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

#define TTF_DIAG_TAIL 4096u

/* ---------- 字体来源：磁盘文件 / Limine module ---------- */

static const char *font_base_name(void) {
    const char *b = TTF_FONT_PATH_DEFAULT;
    for (const char *p = b; *p; p++) if (*p == '/') b = p + 1;
    return b;
}

/* 在 Limine 已加载的 module 里按文件名找字体，返回 module 内存（不拷贝）。 */
static const void *font_module(uint64_t *size_out) {
    if (!g_module_req.response) return 0;
    uint64_t n = g_module_req.response->module_count;
    struct limine_file **mods = g_module_req.response->modules;
    const char *want = font_base_name();
    if (!mods) return 0;
    for (uint64_t i = 0; i < n; i++) {
        struct limine_file *m = mods[i];
        if (!m || !m->address || !m->path) continue;
        const char *base = m->path;
        for (const char *q = base; *q; q++) if (*q == '/') base = q + 1;
        if (kstrcmp(base, want) != 0) continue;
        if (m->size < 1024 || m->size > 8u * 1024u * 1024u) continue;
        if (size_out) *size_out = m->size;
        return (const void *)m->address;
    }
    return 0;
}

static void font_blob_drop(void) {
    if (font_blob && blob_owned) kfree(font_blob);
    font_blob = 0;
    blob_owned = false;
}

/* 把一块字体内存装进 SSFN。owned=false 时直接引用 module 内存：
   Live CD 无盘、无堆拷贝，module 内存由 bootloader 常驻，不会失效。 */
static bool ttf_attach(uint8_t *blob, uint64_t size, bool owned) {
    if (loaded) return true;
    if (!blob || size < 1024 || size > 8u * 1024u * 1024u) {
        if (owned && blob) kfree(blob);
        return false;
    }

    font_blob = blob;
    blob_owned = owned;

    memset(&ctx, 0, sizeof(ctx));
    int rc = ssfn_load(&ctx, font_blob);
    if (rc != SSFN_OK) {
        serial_puts("[SRCore] font: ssfn_load failed rc=");
        serial_putdec((uint64_t)(rc < 0 ? -rc : rc));
        serial_puts("\n");
        font_blob_drop();
        return false;
    }
    rc = ssfn_select(&ctx, SSFN_FAMILY_ANY, 0, 0, TTF_FONT_SIZE);
    if (rc != SSFN_OK) {
        serial_puts("[SRCore] font: ssfn_select failed rc=");
        serial_putdec((uint64_t)(rc < 0 ? -rc : rc));
        serial_puts("\n");
        ssfn_free(&ctx);
        font_blob_drop();
        return false;
    }

    memset(cache, 0, sizeof(cache));
    clock_tick = 0;
    loaded = true;

    /* 用真实字形量行高：ssfn_select() 只设置 ctx.size，不会填 ctx.f，
       所以 "face_h" 必须来自 bbox，而不是 ctx.f->height。 */
    line_px = TTF_FONT_SIZE;
    {
        int w = 0, h = 0, l = 0, t = 0;
        if (ssfn_bbox(&ctx, "A", &w, &h, &l, &t) == SSFN_OK &&
            h >= 8 && h <= 64)
            line_px = (uint32_t)h;
    }

    serial_puts("[SRCore] font: ok ctx.size=");
    serial_putdec((uint64_t)ctx.size);
    serial_puts(" line=");
    serial_putdec(line_px);
    serial_puts(" hash=");
    serial_puthex((uint64_t)font_hash(font_blob, size));
    serial_puts(" heap_free=");
    serial_putdec(heap_free());
    serial_puts("\n");
    return true;
}

bool ttf_init(const char *path) {
    if (loaded) return true;
    if (!fs_ready()) {
        serial_puts("[SRCore] font: fs not ready\n");
        return false;
    }
    if (!path || !path[0]) path = TTF_FONT_PATH_DEFAULT;

    /* st 必须清零：fs_stat 失败时 st.size 是栈上的垃圾，打印/使用它会
       得到随机长度（曾经表现为 REJECT oversize 或读到野地址）。 */
    struct fs_dirent st;
    memset(&st, 0, sizeof(st));

    int sr = fs_stat(path, &st);
    if (sr < 0) {
        serial_puts("[SRCore] font: stat failed rc=");
        serial_putdec((uint64_t)(-sr));
        serial_puts("\n");
        return false;
    }
    serial_puts("[SRCore] font: stat size=");
    serial_putdec(st.size);
    serial_puts(" heap_free=");
    serial_putdec(heap_free());
    serial_puts("\n");
    if (st.size == 0 || st.size > 8u * 1024u * 1024u) {
        serial_puts("[SRCore] font: REJECT (empty/oversize)\n");
        return false;
    }

    uint8_t *blob = (uint8_t *)_kmalloc(st.size, "font", __LINE__);
    if (!blob) {
        serial_puts("[SRCore] font: REJECT (kmalloc failed)\n");
        return false;
    }

    int got = fs_read(path, blob, st.size, 0);
    serial_puts("[SRCore] font: read=");
    serial_putdec((uint64_t)(got < 0 ? 0 : got));
    serial_puts(" want=");
    serial_putdec(st.size);
    serial_puts("\n");
    if (got < 0 || (uint64_t)got != st.size) {
        serial_puts("[SRCore] font: REJECT (short read)\n");
        kfree(blob);
        return false;
    }

    /* 尾巴复读比对：抓「长度对、末尾其实是旧缓存/半页 DMA」这种假成功 */
    {
        static uint8_t tail[TTF_DIAG_TAIL];
        uint64_t off = st.size > TTF_DIAG_TAIL ? st.size - TTF_DIAG_TAIL : 0;
        uint64_t len = st.size - off;
        int g2 = fs_read(path, tail, len, off);
        if (g2 < 0 || (uint64_t)g2 != len)
            serial_puts("[SRCore] font: TAIL REREAD FAILED\n");
        else if (ssfn_memcmp_shim(tail, blob + off, (size_t)len) != 0)
            serial_puts("[SRCore] font: TAIL MISMATCH (disk/cache unstable)\n");
    }

    return ttf_attach(blob, st.size, true);
}

/* Live CD / 无盘：直接以 Limine module 内存为字体，零拷贝、零分配。 */
bool ttf_init_module(void) {
    uint64_t size = 0;
    const void *blob = font_module(&size);
    if (!blob) {
        serial_puts("[SRCore] font: module not found\n");
        return false;
    }
    serial_puts("[SRCore] font: using module, size=");
    serial_putdec(size);
    serial_puts("\n");
    return ttf_attach((uint8_t *)blob, size, false);
}

/* 空盘补给：fs_bootstrap() 只建目录和 /etc/system.srs，不写字体；
   VMware 自建盘因此没有 /usr/share/fonts/siyuan.sfn。这里把 module 里的
   字体一次性落盘，之后 ttf_init_default() 就与 QEMU 预置盘完全一致。
   幂等：文件已存在且尺寸一致直接跳过；尺寸不符说明上次写到一半被打断，
   先删残文件再重写。无盘直接返回（改走 ttf_init_module）。 */
bool font_provision(void) {
    if (!fs_ready()) {
        serial_puts("[SRCore] font: no filesystem, provisioning skipped\n");
        return false;
    }

    uint64_t size = 0;
    const void *blob = font_module(&size);
    if (!blob) {
        serial_puts("[SRCore] font: no module, provisioning skipped\n");
        return false;
    }

    struct fs_dirent st;
    memset(&st, 0, sizeof(st));            /* stat 失败时 st.size 不可读 */
    if (fs_stat(TTF_FONT_PATH_DEFAULT, &st) == FS_OK) {
        if (st.size == size) {
            serial_puts("[SRCore] font: already on disk, provisioning skipped\n");
            return true;
        }
        serial_puts("[SRCore] font: stale file, rewriting\n");
        fs_remove(TTF_FONT_PATH_DEFAULT);
    }

    serial_puts("[SRCore] font: provisioning size=");
    serial_putdec(size);
    serial_puts(" heap_free=");
    serial_putdec(heap_free());
    serial_puts("\n");

    int w = fs_write_file(TTF_FONT_PATH_DEFAULT, blob, size);
    if (w == FS_ERR_NOENT) {
        /* 老盘/外来盘可能没有这个目录：补建一次再写（已存在时返回 EXIST，忽略） */
        fs_create("/usr/share", FS_TYPE_DIR);
        fs_create("/usr/share/fonts", FS_TYPE_DIR);
        w = fs_write_file(TTF_FONT_PATH_DEFAULT, blob, size);
    }
    if (w < 0 || (uint64_t)w != size) {
        serial_puts("[SRCore] font: provision FAILED ");
        serial_puts(fs_strerror(w < 0 ? w : FS_ERR_IO));
        serial_puts("\n");
        fs_remove(TTF_FONT_PATH_DEFAULT);  /* 别把半截文件留给下次启动 */
        return false;
    }
    fs_sync();
    serial_puts("[SRCore] font: provisioned, wrote=");
    serial_putdec((uint64_t)w);
    serial_puts(" heap_free=");
    serial_putdec(heap_free());
    serial_puts("\n");
    return true;
}

/* 按 /etc/system.srs 的 font_path 加载字体；配置缺失或路径失效时回退默认。
   文件名只在这里出现一次（TTF_FONT_PATH_DEFAULT），不在任何调用点写死。 */
bool ttf_init_default(void) {
    char path[FS_PATH_MAX];

    if (!srs_get(TTF_FONT_CONF, "font_path", path, (int)sizeof(path)))
        kstrcpy(path, TTF_FONT_PATH_DEFAULT);

    serial_puts("[SRCore] font path: ");
    serial_puts(path);
    serial_puts("\n");

    if (ttf_init(path)) return true;

    if (kstrcmp(path, TTF_FONT_PATH_DEFAULT) != 0) {
        serial_puts("[SRCore] font fallback: ");
        serial_puts(TTF_FONT_PATH_DEFAULT);
        serial_puts("\n");
        return ttf_init(TTF_FONT_PATH_DEFAULT);
    }
    return false;
}

void ttf_shutdown(void) {
    ttf_cache_clear();
    if (loaded) { ssfn_free(&ctx); loaded = false; }
    font_blob_drop();
}

bool ttf_available(void) { return loaded; }

uint32_t ttf_line_height(void) {
    return (line_px >= 8 && line_px <= 64) ? line_px : TTF_FONT_SIZE;
}

uint32_t ttf_get_text_width(const char *str) {
    if (!str) return 0;
    if (!loaded) {
        /* 无字体：ASCII 单宽、CJK 双宽估算（与 8x16 点阵布局一致） */
        uint32_t w = 0;
        const char *p = str;
        while (*p) {
            uint32_t cp = utf8_decode(&p);
            w += (cp < 0x80) ? FONT_WIDTH : FONT_WIDTH * 2;
        }
        return w;
    }
    int w, h, l, t;
    if (ssfn_bbox(&ctx, str, &w, &h, &l, &t) != SSFN_OK || w <= 0) return 0;
    return (uint32_t)w;
}

/* ---------- 渲染自检（VMware 空白问题的定位工具） ----------
   bbox 只证明字体表里有这个码点；ink 才证明字形轮廓真的被解出来并光栅化。
   box 有值而 ink=0  ==  字体数据在内存里是残的（短读/坏盘/缓存假命中）。 */
static uint32_t probe_ink(const char *utf8, int *ow, int *oh) {
    int w = 0, h = 0, l = 0, t = 0;
    if (ow) *ow = 0;
    if (oh) *oh = 0;
    if (!loaded || !utf8) return 0;

    if (ssfn_bbox(&ctx, utf8, &w, &h, &l, &t) != SSFN_OK || w <= 0 || h <= 0)
        return 0;
    if (ow) *ow = w;
    if (oh) *oh = h;
    if (w > TTF_RENDER_W || h > TTF_RENDER_H) return 0;

    ssfn_buf_t dst;
    dst.ptr = render_mem;
    dst.w = w; dst.h = h; dst.p = (uint16_t)(w * 4);
    dst.x = l; dst.y = t;            /* 与 rasterize 完全相同的画笔原点 */
    dst.fg = 0xFFFFFFFFu;
    dst.bg = 0;
    memset(render_mem, 0, (size_t)(w * h * 4));
    if (ssfn_render(&ctx, &dst, utf8) <= 0) return 0;

    uint32_t ink = 0;
    uint32_t *px = (uint32_t *)render_mem;
    for (int i = 0; i < w * h; i++)
        if ((px[i] >> 24) >= 8) ink++;
    return ink;
}

/* 逐个已加载字体片段做硬验证：高度/基线 + 是否真的含 CJK 码点（U+6B64 此）。
   这是「字体里到底有没有汉字」的唯一硬证据，比看屏幕猜靠谱。 */
static void dump_faces(void) {
    static const char zh[] = "\xE6\xAD\xA4";        /* 此 */
    for (int fam = 0; fam <= SSFN_FAMILY_HAND; fam++) {
        for (int i = 0; i < ctx.len[fam]; i++) {
            const ssfn_font_t *f = ctx.fnt[fam][i];
            if (!f) continue;
            int len = 0;
            uint32_t u = 0;
            uint8_t *hit = _ssfn_c(f, zh, &len, &u);
            serial_puts("[SRCore] face fam=");
            serial_putdec((uint64_t)fam);
            serial_puts(" idx=");
            serial_putdec((uint64_t)i);
            serial_puts(" h=");
            serial_putdec((uint64_t)f->height);
            serial_puts(" base=");
            serial_putdec((uint64_t)f->baseline);
            serial_puts(" type=");
            serial_puthex((uint64_t)f->type);
            serial_puts(" cjk=");
            serial_putdec(hit ? 1 : 0);
            serial_puts("\n");
        }
    }
}

void ttf_selftest(void) {
    static const char *const cp_zh[] = {
        "\xE6\xAD\xA4",   /* 此 */
        "\xE7\x94\xB5",   /* 电 */
        "\xE8\x84\x91",   /* 脑 */
        "\xE7\xB3\xBB",   /* 系 */
        "\xE7\xBB\x9F",   /* 统 */
    };

    serial_puts("[SRCore] font selftest: loaded=");
    serial_putdec(loaded ? 1 : 0);
    serial_puts(" gfx=");
    serial_putdec(gfx_width());
    serial_puts("x");
    serial_putdec(gfx_height());
    serial_puts(" fb=");
    serial_putdec(fb_width());
    serial_puts("x");
    serial_putdec(fb_height());
    serial_puts(" bpp=");
    serial_putdec(fb_bpp());
    serial_puts(" pitch=");
    serial_putdec(fb_pitch());
    serial_puts("\n");
    if (!loaded) {
        serial_puts("[SRCore] font selftest: SKIP (font not loaded)\n");
        return;
    }

    dump_faces();

    int w = 0, h = 0;
    uint32_t ink = probe_ink("A", &w, &h);
    serial_puts("[SRCore] font probe A box=");
    serial_putdec((uint64_t)w);
    serial_puts("x");
    serial_putdec((uint64_t)h);
    serial_puts(" ink=");
    serial_putdec(ink);
    serial_puts("\n");

    for (int i = 0; i < (int)(sizeof(cp_zh) / sizeof(cp_zh[0])); i++) {
        const char *p = cp_zh[i];
        const char *q = p;
        uint32_t cp = utf8_decode(&q);
        ink = probe_ink(p, &w, &h);
        serial_puts("[SRCore] font probe U+");
        serial_puthex(cp);
        serial_puts(" box=");
        serial_putdec((uint64_t)w);
        serial_puts("x");
        serial_putdec((uint64_t)h);
        serial_puts(" ink=");
        serial_putdec(ink);
        serial_puts(ink ? "\n" : "  <-- EMPTY GLYPH\n");
    }
}


/* 字体没加载时的最后兜底：只画 ASCII 点阵，CJK 留双宽空位。
   绝不逐字节画多字节 UTF-8，也绝不回退到 font_draw_str_bg。 */
static void ascii_fallback(int32_t x, int32_t y, const char *str,
                           uint32_t fg, uint32_t bg) {
    const char *p = str;
    while (*p) {
        uint32_t cp = utf8_decode(&p);
        if (cp >= 32 && cp < 127) {
            if (bg) gfx_rect(x, y, FONT_WIDTH, FONT_HEIGHT, bg);
            char one[2] = { (char)cp, 0 };
            font_draw_str(x, y, one, fg);
            x += FONT_WIDTH;
        } else if (cp >= 127) {
            x += FONT_WIDTH * 2;            /* 无 CJK 字形：占位，不画乱码 */
        }
    }
}

void ttf_draw_text(int32_t x, int32_t y, const char *str,
                   uint32_t fg_color, uint32_t bg_color) {
    if (!str) return;
    if (!loaded) { ascii_fallback(x, y, str, fg_color, bg_color); return; }

    /* Y 只用调用方给的 y，绝不参与 face_h / ctx.f 的任何计算；
       负值（度量异常时的 0xFFFF...F0）直接夹到 0 继续画。 */
    if (y < 0) y = 0;

    /* 背景：整串只填一次，避免逐字填色擦掉相邻字模的抗锯齿边缘 */
    if (bg_color) {
        int bw = 0, bh = 0, bl = 0, bt = 0;
        if (ssfn_bbox(&ctx, str, &bw, &bh, &bl, &bt) == SSFN_OK && bw > 0) {
            uint32_t lh = (bh > 0) ? (uint32_t)bh : TTF_FONT_SIZE;
            gfx_rect(x, y, (uint32_t)bw, lh, bg_color);
        }
    }

    const char *p = str;
    int32_t cur_x = x;

    while (*p) {
        uint32_t cp = utf8_decode(&p);
        if (cp == 0) break;
        if (cp == '\n' || cp == '\r') continue;

        glyph_t *g = cache_lookup(cp);
        if (!g) {
            g = cache_evict(cp);
            if (rasterize(cp, g) != GLYPH_OK || !g->gray) {
                /* 字库里没有这个码点：CJK 画空心方框占位（绝不静默留白） */
                if (cp >= 0x80) {
                    gfx_rect(cur_x, y + 2, 8, 1, fg_color);
                    gfx_rect(cur_x, y + 13, 8, 1, fg_color);
                    gfx_rect(cur_x, y + 2, 1, 12, fg_color);
                    gfx_rect(cur_x + 7, y + 2, 1, 12, fg_color);
                }
                cur_x += 8;                         /* 缺字给默认宽度 */
                continue;
            }
        }

        /* 强制按行逐像素绘制：Y 就是传入的 y，不加任何 face/baseline 偏移 */
        for (uint32_t row = 0; row < g->h; row++) {
            for (uint32_t col = 0; col < g->w; col++) {
                uint8_t a = g->gray[row * g->w + col];
                if (a == 0) continue;

                int32_t px = cur_x + (int32_t)col;
                int32_t py = y + (int32_t)row;

                if (px < 0 || px >= (int32_t)gfx_width() ||
                    py < 0 || py >= (int32_t)gfx_height()) continue;

                uint32_t bg = gfx_get_pixel(px, py);
                uint32_t r  = (((fg_color >> 16) & 0xFF) * a +
                               ((bg >> 16) & 0xFF) * (255 - a)) / 255;
                uint32_t gr = (((fg_color >> 8) & 0xFF) * a +
                               ((bg >> 8) & 0xFF) * (255 - a)) / 255;
                uint32_t b  = ((fg_color & 0xFF) * a +
                               (bg & 0xFF) * (255 - a)) / 255;
                gfx_pixel(px, py, (r << 16) | (gr << 8) | b);
            }
        }

        cur_x += g->advance ? g->advance : 8;
    }
}