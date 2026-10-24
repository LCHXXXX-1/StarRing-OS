#include "boot/sysinit.h"
#include "drivers/rtc.h"
#include "drivers/serial.h"
#include "gfx/gfx.h"
#include "gfx/gui.h"

/* ============================================================
 *  状态机实现
 *    Step 1  SYS_ST_RTC   读 RTC（rtc.c 内部已加超时，最坏也就自旋
 *                          RTC_WAIT_MAX 次就返回 false）
 *    Step 2  SYS_ST_BG    分块预计算渐变壁纸，每帧只算 BG_CHUNK 行
 *    Step 3  SYS_ST_DONE  标记完成
 * ============================================================ */

#define BG_CHUNK 48u            /* 每帧预计算行数：够小 -> 跑马灯不掉帧 */

enum {
    SYS_ST_RTC = 0,
    SYS_ST_BG,
    SYS_ST_FINISH,
    SYS_ST_DONE
};

static int          sys_state = SYS_ST_RTC;
static volatile bool sys_complete = false;

static int  rtc_h = -1, rtc_m = 0, rtc_s = 0;
static int  rtc_day = 0, rtc_mon = 0, rtc_year = 0;
static bool rtc_time_ok = false;
static bool rtc_date_ok = false;

uint32_t bg_gradient_cache[BG_CACHE_ROWS];
volatile uint32_t bg_cache_rows = 0;

/* ---- 渐变增量状态（16.16 定点，全程无除法，除法只在开段时算一次） ---- */
static uint32_t bg_cache_h  = 0;    /* 缓存对应的高度 */
static uint32_t bg_next_row = 0;    /* 下一待算行 */
static uint32_t bg_seg_to   = 0;    /* 当前段的结束行（不含） */
static int      bg_seg_id   = 0;    /* 0=未开段 1=上段 2=下段 */
static int32_t  bg_fr = 0, bg_fg = 0, bg_fb = 0;   /* 当前颜色 16.16 */
static int32_t  bg_dr = 0, bg_dg = 0, bg_db = 0;   /* 每行增量   16.16 */

/* 缓存实际能覆盖的行数（受 BG_CACHE_ROWS 限制） */
static uint32_t bg_target(void) {
    uint32_t sh = gfx_height();
    if (sh > BG_CACHE_ROWS) sh = BG_CACHE_ROWS;
    return sh;
}

/* 开一段 [from, to) 的线性渐变 a -> b；步长按整段跨度算，跨帧续算不改变斜率 */
static void bg_seg_begin(uint32_t from, uint32_t to, uint32_t a, uint32_t b) {
    uint32_t span = (to > from) ? (to - from) : 1u;
    int32_t ar = (int32_t)((a >> 16) & 0xFF), ag = (int32_t)((a >> 8) & 0xFF),
            ab = (int32_t)(a & 0xFF);
    int32_t br = (int32_t)((b >> 16) & 0xFF), bg_ = (int32_t)((b >> 8) & 0xFF),
            bb = (int32_t)(b & 0xFF);

    bg_fr = ar << 16;
    bg_fg = ag << 16;
    bg_fb = ab << 16;
    bg_dr = ((br - ar) << 16) / (int32_t)span;
    bg_dg = ((bg_ - ag) << 16) / (int32_t)span;
    bg_db = ((bb - ab) << 16) / (int32_t)span;

    bg_next_row = from;
    bg_seg_to   = to;
}

/* 计算 n 行（n 可为很大的数以一次性算完） */
static void bg_chunk(uint32_t n) {
    uint32_t sh = bg_target();
    if (sh == 0) return;                 /* gfx 还没就绪 */

    if (bg_cache_h != sh) {              /* 分辨率变化 -> 全部重来 */
        bg_cache_h  = sh;
        bg_cache_rows = 0;
        bg_next_row = 0;
        bg_seg_id   = 0;
    }

    uint32_t mid = sh / 2;                 /* sh==1 时 mid==0：整屏按下半段处理 */

    while (n && bg_cache_rows < sh) {
        /* 需要开新段：还没开过，或者已经算到段尾 */
        if (bg_seg_id == 0 || bg_next_row >= bg_seg_to) {
            if (bg_cache_rows < mid) {
                bg_seg_id = 1;
                bg_seg_begin(bg_cache_rows, mid,
                             GUI_COL_DESKTOP_TOP, GUI_COL_DESKTOP_MID);
            } else {
                bg_seg_id = 2;
                bg_seg_begin(bg_cache_rows, sh,
                             GUI_COL_DESKTOP_MID, GUI_COL_DESKTOP_BOT);
            }
            if (bg_next_row < bg_cache_rows) bg_next_row = bg_cache_rows;
        }

        uint32_t from = bg_next_row;
        uint32_t stop = from + n;
        if (stop > bg_seg_to)      stop = bg_seg_to;
        if (stop > sh)             stop = sh;
        if (stop <= from) { n = 1; continue; }   /* 防死循环：只能重新开段 */

        for (; bg_next_row < stop; bg_next_row++) {
            uint32_t r = (uint32_t)((bg_fr >> 16) & 0xFF);
            uint32_t g = (uint32_t)((bg_fg >> 16) & 0xFF);
            uint32_t b = (uint32_t)((bg_fb >> 16) & 0xFF);
            bg_gradient_cache[bg_next_row] = (r << 16) | (g << 8) | b;
            bg_fr += bg_dr;
            bg_fg += bg_dg;
            bg_fb += bg_db;
        }
        if (stop > bg_cache_rows) bg_cache_rows = stop;

        uint32_t did = stop - from;
        n = (n > did) ? (n - did) : 0;
    }
}

bool bg_cache_valid(void) {
    uint32_t sh = bg_target();
    if (sh == 0) return false;
    if (bg_cache_h == sh && bg_cache_rows >= sh) return true;
    /* 冷路径：遮罩没跑完就改了分辨率，或干脆没经过遮罩 —— 一次性补齐 */
    bg_cache_h = sh;
    bg_cache_rows = 0;
    bg_next_row = 0;
    bg_seg_id = 0;
    bg_chunk(0xFFFFFFFFu);
    return bg_cache_rows >= sh;
}

bool system_init_step(void) {
    switch (sys_state) {
    case SYS_ST_RTC:
        serial_puts("[SRCore] sysinit: reading RTC\n");
        rtc_time_ok = rtc_read(&rtc_h, &rtc_m, &rtc_s);
        if (!rtc_time_ok) rtc_h = -1;              /* 上层回退开机时长 */
        rtc_date_ok = rtc_read_date(&rtc_day, &rtc_mon, &rtc_year);
        if (!rtc_date_ok) rtc_day = 0;             /* 上层不显示日期 */
        serial_puts(rtc_time_ok ? "[SRCore] sysinit: RTC ok\n"
                                : "[SRCore] sysinit: RTC timeout\n");
        sys_state = SYS_ST_BG;
        return false;

    case SYS_ST_BG:
        bg_chunk(BG_CHUNK);
        if (bg_cache_rows >= bg_target()) {
            sys_state = SYS_ST_FINISH;
            serial_puts("[SRCore] sysinit: wallpaper cached\n");
        }
        return false;

    case SYS_ST_FINISH:
        sys_state = SYS_ST_DONE;
        sys_complete = true;
        return true;

    default:                        /* SYS_ST_DONE：已完成，幂等返回 */
        sys_complete = true;
        return true;
    }
}

bool system_init_done(void) { return sys_complete; }

void system_init_force_done(void) {
    sys_state = SYS_ST_DONE;
    sys_complete = true;
    bg_cache_valid();                /* 别让桌面退化成逐行 mix() */
}

/* 手写 "Wallpaper 64%"，不依赖 vsprintf；全静态缓冲、纯整数 */
static const char *bg_percent_text(char *buf, int max) {
    uint32_t sh = bg_target();
    uint32_t r  = bg_cache_rows;
    if (r > sh) r = sh;
    uint32_t pct = sh ? (r * 100u) / sh : 0u;    /* 32 位整数，无浮点 */

    const char *pre = "Wallpaper ";
    int p = 0;
    for (int i = 0; pre[i] && p + 6 < max; i++) buf[p++] = pre[i];

    char d[4]; int nd = 0;
    if (pct == 0) d[nd++] = '0';
    while (pct && nd < 4) { d[nd++] = (char)('0' + pct % 10u); pct /= 10u; }
    while (nd && p + 2 < max) buf[p++] = d[--nd];
    if (p + 1 < max) buf[p++] = '%';
    buf[p] = 0;
    return buf;
}

const char *system_init_label(void) {
    static char buf[24];        /* "Wallpaper 100%" */
    switch (sys_state) {
    case SYS_ST_RTC: return "Reading real-time clock";
    case SYS_ST_BG:  return bg_percent_text(buf, sizeof(buf));
    case SYS_ST_FINISH: return "Starting desktop";
    default:         return "Ready";
    }
}

bool sysinit_rtc_time(int *hour, int *min, int *sec) {
    if (!rtc_time_ok) return false;
    if (hour) *hour = rtc_h;
    if (min)  *min  = rtc_m;
    if (sec)  *sec  = rtc_s;
    return true;
}

bool sysinit_rtc_date(int *day, int *month, int *year) {
    if (!rtc_date_ok) return false;
    if (day)   *day   = rtc_day;
    if (month) *month = rtc_mon;
    if (year)  *year  = rtc_year;
    return true;
}
