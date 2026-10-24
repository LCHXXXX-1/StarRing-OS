#include "login.h"

#include <stdint.h>
#include <stdbool.h>

#include "gfx/gfx.h"
#include "gfx/gui.h"
#include "gfx/font.h"
#include "boot/sysinit.h"
#include "drivers/timer.h"
#include "drivers/keyboard.h"
#include "drivers/input.h"
#include "fs/fs.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "apps/terminal.h"

/* ============================================================
 *  全屏登录 / 锁屏
 *
 *  状态机：
 *      LS_LOCK      锁屏，显示时钟；双击或任意按键进入密码页
 *      LS_ANIM_SLIDE 24 帧上滑动画（tick 驱动）
 *      LS_PASSWORD  密码输入
 *      LS_PICKER    用户选择
 *      LS_CREATE    新建用户
 *      LS_DONE      认证成功，login_run() 收尾后返回
 *
 *  约束：
 *      - 没有堆分配 / libc / 格式化输出；
 *      - 所有绘制只写 GUI 后台缓冲，由 desktop_tick 分流里的 composite()
 *        统一上屏；
 *      - 鼠标/键盘回调只改状态并 gui_damage*，不做耗时操作。
 * ============================================================ */

#define LOGIN_MAX_USERS   8
#define LOGIN_NAME_MAX    16
#define LOGIN_PASS_MAX    32
#define LOGIN_ANIM_FRAMES 24
#define LOGIN_DB_MAGIC    0x49474F4Cu   /* 'LOGI' */
#define LOGIN_DB_VERSION  1u
#define LOGIN_ERR_COLOR   0x00FF6B5E

struct login_user {
    char     name[LOGIN_NAME_MAX + 1];   /* 17 字节，NUL 结尾 */
    uint8_t  pad0[3];
    uint32_t pw_hash;                    /* FNV-1a 32-bit，0 = 无密码 */
    uint8_t  flags;
    uint8_t  pad1[3];
} __attribute__((packed));

struct login_db {
    uint32_t magic;
    uint32_t version;
    uint32_t user_count;
    uint8_t  pad[4];
    struct login_user users[LOGIN_MAX_USERS];
} __attribute__((packed));

_Static_assert(sizeof(struct login_user) == 28, "login_user must be 28 bytes");
_Static_assert(sizeof(struct login_db) == 240, "login_db must be 240 bytes");

enum {
    LS_LOCK = 0,
    LS_PASSWORD,
    LS_PICKER,
    LS_CREATE,
    LS_ANIM_SLIDE,
    LS_DONE
};

/* 按钮标识（仅 login.c 内部使用） */
enum {
    HB_NONE = 0,
    HB_SIGNIN,
    HB_SWITCH,
    HB_NEWUSER,
    HB_BACK,
    HB_CREATE,
    HB_CANCEL
};

static struct login_db db;
static bool guest_mode;                 /* fs_ready()==false：只读访客 */
static int  state;
static int  cur_user;

static char pass_buf[LOGIN_PASS_MAX + 1];
static int  pass_len;
static char err_msg[64];

static char create_name[LOGIN_NAME_MAX + 1];
static int  create_name_len;
static char create_pass[LOGIN_PASS_MAX + 1];
static int  create_pass_len;
static char create_confirm[LOGIN_PASS_MAX + 1];
static int  create_confirm_len;
static int  focus;                      /* 0=username 1=password 2=confirm */

static int32_t cursor_x, cursor_y;
static uint8_t prev_buttons;
static uint64_t last_click_tick;
static bool     last_click_valid;

static int32_t hover_row;
static int32_t hover_btn;

static int anim_frame;
static int anim_target;

static int clock_h, clock_m, clock_s;
static int clock_day, clock_month, clock_year;

static void (*post_login_hook)(void);

/* ---------------- 小工具 ---------------- */

static uint32_t login_mix(uint32_t a, uint32_t b, uint32_t num, uint32_t den) {
    uint32_t ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    uint32_t br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    uint32_t r = (ar * (den - num) + br * num) / den;
    uint32_t g = (ag * (den - num) + bg * num) / den;
    uint32_t bl = (ab * (den - num) + bb * num) / den;
    return (r << 16) | (g << 8) | bl;
}

/* 与 gui.c 的 wall_color() 同算法（不引用它的 static 函数）。 */
static uint32_t login_wall_color(int32_t y) {
    int32_t sh = (int32_t)gfx_height();
    if (sh <= 1) sh = 2;
    int32_t mid = sh / 2;
    if (y < mid) {
        uint32_t t = (uint32_t)(y < 0 ? 0 : y);
        return login_mix(GUI_COL_DESKTOP_TOP, GUI_COL_DESKTOP_MID,
                         t, (uint32_t)mid);
    }
    {
        uint32_t t = (uint32_t)(y < mid ? mid : y - mid);
        return login_mix(GUI_COL_DESKTOP_MID, GUI_COL_DESKTOP_BOT,
                         t, (uint32_t)(sh - mid));
    }
}

static void draw_wall(void) {
    int32_t sw = (int32_t)gfx_width();
    int32_t sh = (int32_t)gfx_height();

    /* 与桌面共用同一份预计算渐变：一次调用铺满，登录页每次合成不再
       逐行做 3 次除法（老机器锁屏动画的主要开销）。 */
    if (bg_cache_valid()) {
        gfx_fill_row_colors(0, 0, (uint32_t)sw, (uint32_t)sh,
                            bg_gradient_cache, bg_cache_rows);
        return;
    }
    for (int32_t y = 0; y < sh; y++)
        gui_fill(0, y, (uint32_t)sw, 1, login_wall_color(y));
}

static int text_width(const char *s) {
    return (int)gui_str_width(s);
}

static void draw_centered(int32_t y, const char *s, uint32_t color) {
    int32_t sw = (int32_t)gfx_width();
    gui_text((sw - text_width(s)) / 2, y, s, color, 0);
}

static void draw_centered_scaled(int32_t y, const char *s,
                                 uint32_t color, uint32_t scale) {
    int32_t sw = (int32_t)gfx_width();
    int32_t w = (int32_t)(kstrlen(s) * FONT_WIDTH * scale);
    font_draw_str_scaled((sw - w) / 2, y, s, color, scale);
}

static void copy_name(char *dst, const char *src, int max) {
    int i = 0;
    for (; i < max && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static bool name_taken(const char *name) {
    for (uint32_t i = 0; i < db.user_count; i++)
        if (kstrcmp(db.users[i].name, name) == 0) return true;
    return false;
}

static char initial_of(const char *name) {
    char c = name[0];
    if (!c) return '?';
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return c;
}

/* 用整数像素中心画一个外接框严格为 2r x 2r 的实心圆。 */
static void fill_circle(int32_t cx, int32_t cy, int32_t r, uint32_t color) {
    int32_t d = r * 2;
    int32_t c2 = d - 1;
    int32_t rr = d * d;
    for (int32_t py = 0; py < d; py++) {
        int32_t dy2 = 2 * py - c2;
        int32_t rem = rr - dy2 * dy2;
        int32_t h = 1;
        if (rem < 1) continue;
        while ((h + 2) * (h + 2) <= rem) h += 2;
        int32_t x0 = (c2 - h) / 2;
        int32_t x1 = (c2 + h) / 2;
        gui_fill(cx - r + x0, cy - r + py,
                 (uint32_t)(x1 - x0 + 1), 1, color);
    }
}

static void draw_avatar(int32_t cx, int32_t cy, int32_t r,
                        const char *name, bool large) {
    char s[2];
    s[0] = initial_of(name);
    s[1] = 0;
    fill_circle(cx, cy, r, GUI_COL_ACCENT);
    if (large)
        font_draw_str_scaled(cx - 8, cy - 16, s, GUI_COL_WHITE, 2);
    else
        font_draw_str(cx - 4, cy - 8, s, GUI_COL_WHITE);
}

static void draw_cursor_shape(int32_t x, int32_t y, uint32_t color) {
    for (int j = 0; j < 12; j++) {
        int w = (j < 9) ? (j / 3 + 1) : (12 - j);
        if (w < 1) w = 1;
        gui_fill(x, y + j, (uint32_t)w, 1, color);
    }
    gui_fill(x + 2, y + 9, 1, 4, color);
    gui_fill(x + 4, y + 11, 1, 2, color);
}

static void draw_cursor(void) {
    /* 登录界面自己画指针；GUI 的桌面指针被停在左上角，不在卡片上乱跳。 */
    draw_cursor_shape(cursor_x + 1, cursor_y + 1, 0x00000000);
    draw_cursor_shape(cursor_x, cursor_y, GUI_COL_WHITE);
}

static void damage_cursor_at(int32_t x, int32_t y) {
    gui_damage_rect(x - 1, y - 1, 16, 18);
}

/* ---------------- 时间/日期 ---------------- */

static void refresh_clock(void) {
    /* 不再直接读 CMOS：时间与日期取开机遮罩里读好的成品，
       之后用 PIT tick（100Hz）做纯整数推算，锁屏时钟每秒刷新一次。 */
    static int  inited = 0;
    static int  bh, bm, bs;
    static uint64_t base_tick = 0;

    if (!inited) {
        inited = 1;
        if (sysinit_rtc_time(&bh, &bm, &bs)) {
            base_tick = timer_ticks();
            sysinit_rtc_date(&clock_day, &clock_month, &clock_year);
        } else {
            bh = 0; bm = 0; bs = 0;           /* RTC 超时：从 00:00:00 起算 */
            clock_day = 1; clock_month = 1; clock_year = 2000;
        }
        if (clock_day < 1) { clock_day = 1; clock_month = 1; clock_year = 2000; }
    }

    uint64_t el = timer_ticks() - base_tick;          /* tick -> 秒，无浮点 */
    uint64_t tot = (uint64_t)bh * 3600u + (uint64_t)bm * 60u + (uint64_t)bs
                 + el / 100u;
    tot %= 86400u;
    clock_h = (int)(tot / 3600u);
    clock_m = (int)((tot / 60u) % 60u);
    clock_s = (int)(tot % 60u);
}

static void fmt_time(char *out) {
    int h = clock_h, m = clock_m;
    if (h < 0) h = 0;
    if (h > 99) h = 99;
    if (m < 0) m = 0;
    if (m > 99) m = 99;
    out[0] = (char)('0' + h / 10);
    out[1] = (char)('0' + h % 10);
    out[2] = ':';
    out[3] = (char)('0' + m / 10);
    out[4] = (char)('0' + m % 10);
    out[5] = 0;
}

static void fmt_date(char *out) {
    int y = clock_year, mo = clock_month, d = clock_day;
    if (y < 0) y = 0;
    if (y > 9999) y = 9999;
    if (mo < 0) mo = 0;
    if (mo > 99) mo = 99;
    if (d < 0) d = 0;
    if (d > 99) d = 99;
    out[0] = (char)('0' + (y / 1000) % 10);
    out[1] = (char)('0' + (y / 100) % 10);
    out[2] = (char)('0' + (y / 10) % 10);
    out[3] = (char)('0' + y % 10);
    out[4] = '-';
    out[5] = (char)('0' + mo / 10);
    out[6] = (char)('0' + mo % 10);
    out[7] = '-';
    out[8] = (char)('0' + d / 10);
    out[9] = (char)('0' + d % 10);
    out[10] = 0;
}

/* ---------------- 用户数据库 ---------------- */

static void db_default(void) {
    memset(&db, 0, sizeof(db));
    db.magic = LOGIN_DB_MAGIC;
    db.version = LOGIN_DB_VERSION;
    db.user_count = 1;
    copy_name(db.users[0].name, "guest", LOGIN_NAME_MAX);
    db.users[0].pw_hash = 0;
    db.users[0].flags = 0;
}

static bool db_valid(const struct login_db *d) {
    if (d->magic != LOGIN_DB_MAGIC || d->version != LOGIN_DB_VERSION)
        return false;
    if (d->user_count < 1 || d->user_count > LOGIN_MAX_USERS)
        return false;
    for (uint32_t i = 0; i < d->user_count; i++) {
        bool term = false;
        for (int j = 0; j <= LOGIN_NAME_MAX; j++) {
            if (d->users[i].name[j] == 0) { term = true; break; }
        }
        if (!term || d->users[i].name[0] == 0) return false;
    }
    return true;
}

static bool db_save(void) {
    if (guest_mode) return false;
    int n = fs_write_file("/etc/users.dat", &db, sizeof(db));
    return n == (int)sizeof(db);
}

static void db_load(void) {
    guest_mode = !fs_ready();
    db_default();
    if (guest_mode) return;

    fs_create("/etc", FS_TYPE_DIR);
    fs_create("/etc/users.dat", FS_TYPE_FILE);

    struct login_db tmp;
    memset(&tmp, 0, sizeof(tmp));
    int n = fs_read("/etc/users.dat", &tmp, sizeof(tmp), 0);
    if (n == (int)sizeof(tmp) && db_valid(&tmp)) {
        memcpy(&db, &tmp, sizeof(db));
        return;
    }

    /* 文件不存在或损坏：重建默认库并落盘。 */
    db_default();
    db_save();
}

static uint32_t fnv1a32(const char *s, int len) {
    if (len <= 0) return 0;            /* 空密码约定为 0 */
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

static void set_error(const char *s) {
    kstrcpy(err_msg, s);
}

static void clear_error(void) {
    err_msg[0] = 0;
}

static void clear_password(void) {
    pass_len = 0;
    pass_buf[0] = 0;
}

/* ---------------- 几何 ---------------- */

static void center_rect(int32_t w, int32_t h, int32_t *x, int32_t *y) {
    *x = ((int32_t)gfx_width() - w) / 2;
    *y = ((int32_t)gfx_height() - h) / 2;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
}

static void password_geom(int32_t *x, int32_t *y) {
    center_rect(360, 260, x, y);
}

static void picker_geom(int32_t *x, int32_t *y, int32_t *h) {
    int n = (int)db.user_count;
    if (n < 1) n = 1;
    *h = 80 + n * 44 + 60;
    center_rect(360, *h, x, y);
}

static void create_geom(int32_t *x, int32_t *y) {
    center_rect(360, 300, x, y);
}

static bool picker_row_rect(int index, int32_t *x, int32_t *y,
                            int32_t *w, int32_t *h) {
    if (index < 0 || (uint32_t)index >= db.user_count) return false;
    int32_t cx, cy, ch;
    picker_geom(&cx, &cy, &ch);
    (void)ch;
    *x = cx + 16;
    *y = cy + 80 + index * 44;
    *w = 328;
    *h = 44;
    return true;
}

static bool button_rect(int id, int32_t *x, int32_t *y,
                        int32_t *w, int32_t *h) {
    if (id == HB_SIGNIN && state == LS_PASSWORD) {
        int32_t cx, cy;
        password_geom(&cx, &cy);
        *x = cx + 108; *y = cy + 200; *w = 100; *h = 26;
        return true;
    }
    if (id == HB_SWITCH && state == LS_PASSWORD) {
        int32_t cx, cy;
        password_geom(&cx, &cy);
        *x = cx + 216; *y = cy + 200; *w = 110; *h = 26;
        return true;
    }
    if (id == HB_NEWUSER && state == LS_PICKER) {
        int32_t cx, cy, ch;
        picker_geom(&cx, &cy, &ch);
        *x = cx + 16; *y = cy + ch - 44; *w = 110; *h = 26;
        return true;
    }
    if (id == HB_BACK && state == LS_PICKER) {
        int32_t cx, cy, ch;
        picker_geom(&cx, &cy, &ch);
        *x = cx + 136; *y = cy + ch - 44; *w = 80; *h = 26;
        return true;
    }
    if (id == HB_CREATE && state == LS_CREATE) {
        int32_t cx, cy;
        create_geom(&cx, &cy);
        *x = cx + 74; *y = cy + 244; *w = 120; *h = 26;
        return true;
    }
    if (id == HB_CANCEL && state == LS_CREATE) {
        int32_t cx, cy;
        create_geom(&cx, &cy);
        *x = cx + 206; *y = cy + 244; *w = 80; *h = 26;
        return true;
    }
    return false;
}

static int hit_button(void) {
    if (state == LS_PASSWORD) {
        int32_t x, y, w, h;
        if (button_rect(HB_SIGNIN, &x, &y, &w, &h) &&
            gui_hit(cursor_x, cursor_y, x, y, (uint32_t)w, (uint32_t)h))
            return HB_SIGNIN;
        if (button_rect(HB_SWITCH, &x, &y, &w, &h) &&
            gui_hit(cursor_x, cursor_y, x, y, (uint32_t)w, (uint32_t)h))
            return HB_SWITCH;
    } else if (state == LS_PICKER) {
        int32_t x, y, w, h;
        if (!guest_mode &&
            button_rect(HB_NEWUSER, &x, &y, &w, &h) &&
            gui_hit(cursor_x, cursor_y, x, y, (uint32_t)w, (uint32_t)h))
            return HB_NEWUSER;
        if (button_rect(HB_BACK, &x, &y, &w, &h) &&
            gui_hit(cursor_x, cursor_y, x, y, (uint32_t)w, (uint32_t)h))
            return HB_BACK;
    } else if (state == LS_CREATE) {
        int32_t x, y, w, h;
        if (button_rect(HB_CREATE, &x, &y, &w, &h) &&
            gui_hit(cursor_x, cursor_y, x, y, (uint32_t)w, (uint32_t)h))
            return HB_CREATE;
        if (button_rect(HB_CANCEL, &x, &y, &w, &h) &&
            gui_hit(cursor_x, cursor_y, x, y, (uint32_t)w, (uint32_t)h))
            return HB_CANCEL;
    }
    return HB_NONE;
}

static int picker_row_at(void) {
    if (state != LS_PICKER) return -1;
    for (uint32_t i = 0; i < db.user_count; i++) {
        int32_t x, y, w, h;
        if (picker_row_rect((int)i, &x, &y, &w, &h) &&
            gui_hit(cursor_x, cursor_y, x, y, (uint32_t)w, (uint32_t)h))
            return (int)i;
    }
    return -1;
}

/* ---------------- 绘制：卡片/按钮 ---------------- */

static void draw_card_base(int32_t x, int32_t y, int32_t w, int32_t h,
                           const char *title) {
    gui_fill(x, y, (uint32_t)w, (uint32_t)h, GUI_COL_PANEL);
    gui_frame(x, y, (uint32_t)w, (uint32_t)h, GUI_COL_BORDER);
    if (title) {
        gui_fill(x + 1, y + 1, (uint32_t)(w - 2), 28, GUI_COL_PANEL_LIGHT);
        gui_fill(x + 1, y + 29, (uint32_t)(w - 2), 1, GUI_COL_BORDER);
        gui_text(x + 10, y + 6, title, GUI_COL_TEXT, GUI_COL_PANEL_LIGHT);
    }
}

static void draw_btn(int32_t x, int32_t y, int32_t w, int32_t h,
                     const char *label, bool hot, bool disabled) {
    uint32_t bg = disabled ? GUI_COL_BTN_DISABLED
                           : (hot ? GUI_COL_BTN_HOVER : GUI_COL_BTN);
    uint32_t border = disabled ? 0x002A3A4C
                               : (hot ? GUI_COL_ACCENT : GUI_COL_BORDER);
    int32_t tw = text_width(label);
    int32_t tx = x + (w - tw) / 2;
    int32_t ty = y + (h - FONT_HEIGHT) / 2;
    if (tx < x + 3) tx = x + 3;
    gui_round_fill(x, y, (uint32_t)w, (uint32_t)h, bg, GUI_COL_PANEL);
    gui_frame(x, y, (uint32_t)w, (uint32_t)h, border);
    gui_text(tx, ty, label,
             disabled ? GUI_COL_TEXT_DIM : GUI_COL_TEXT, bg);
}

static void draw_lock_content(int32_t dy) {
    char tbuf[8];
    char dbuf[16];
    int32_t sh = (int32_t)gfx_height();
    fmt_time(tbuf);
    fmt_date(dbuf);
    draw_centered_scaled(sh / 3 + dy, tbuf, GUI_COL_WHITE, 4);
    draw_centered(sh / 3 + dy + 4 * FONT_HEIGHT + 16, dbuf,
                  GUI_COL_TEXT_DIM);
    draw_centered(sh - 78 + dy, "双击解锁", GUI_COL_TEXT_DIM);
}

static void draw_password_card(int32_t cx, int32_t cy, int hov) {
    int32_t bx = cx + 108, by = cy + 96;
    int32_t sx = cx + 108, sy = cy + 200;
    const char *name = db.users[cur_user].name;

    draw_card_base(cx, cy, 360, 260, 0);

    draw_avatar(cx + 54, cy + 54, 36, name, true);

    /* 用户名（2x，最多 14 字符，避免撞出卡片） */
    {
        char nb[15];
        int i = 0;
        for (; i < 14 && name[i]; i++) nb[i] = name[i];
        nb[i] = 0;
        font_draw_str_scaled(cx + 108, cy + 32, nb, GUI_COL_TEXT, 2);
    }

    /* 密码框 */
    gui_fill(bx, by, 220, 28, GUI_COL_PANEL_LIGHT);
    gui_frame(bx, by, 220, 28, GUI_COL_ACCENT);
    {
        char stars[LOGIN_PASS_MAX + 1];
        int n = pass_len;
        if (n > 26) n = 26;           /* 220px 宽最多 26 个点阵 '*' */
        for (int i = 0; i < n; i++) stars[i] = '*';
        stars[n] = 0;
        if (n > 0)
            gui_text(bx + 7, by + 6, stars, GUI_COL_TEXT, GUI_COL_PANEL_LIGHT);
    }

    /* 错误提示 */
    gui_fill(cx + 108, cy + 132, 220, 16, GUI_COL_PANEL);
    if (err_msg[0])
        gui_text(cx + 108, cy + 132, err_msg, LOGIN_ERR_COLOR, GUI_COL_PANEL);

    /* 按钮 */
    draw_btn(sx, sy, 100, 26, "登录", hov == HB_SIGNIN, false);
    draw_btn(cx + 216, sy, 110, 26, "切换用户", hov == HB_SWITCH, false);

    if (guest_mode) {
        const char *msg = "只读访客模式（无文件系统）";
        int32_t tw = text_width(msg);
        gui_fill(cx + 10, cy + 234, 340, 18, GUI_COL_PANEL);
        gui_text(cx + (360 - tw) / 2, cy + 236, msg,
                 GUI_COL_TEXT_DIM, GUI_COL_PANEL);
    }
}

static void draw_picker_card(void) {
    int32_t cx, cy, ch;
    int32_t rows_y;
    picker_geom(&cx, &cy, &ch);
    draw_card_base(cx, cy, 360, ch, "切换用户");

    if (guest_mode) {
        gui_text(cx + 20, cy + 38, "只读访客模式（无文件系统）",
                 GUI_COL_TEXT_DIM, GUI_COL_PANEL);
    }

    rows_y = cy + 80;
    for (uint32_t i = 0; i < db.user_count; i++) {
        int32_t x = cx + 16;
        int32_t y = rows_y + (int32_t)i * 44;
        bool selected = ((int)i == cur_user);
        bool hot = ((int)i == hover_row);
        uint32_t bg = selected ? GUI_COL_SELECT
                               : (hot ? GUI_COL_SELECT_HOVER : GUI_COL_PANEL);

        gui_fill(x, y, 328, 44, bg);
        if (selected) gui_frame(x, y, 328, 44, GUI_COL_ACCENT);
        draw_avatar(x + 20, y + 22, 14, db.users[i].name, false);
        font_draw_str_bg(x + 44, y + 14, db.users[i].name,
                         GUI_COL_TEXT, bg);
    }

    draw_btn(cx + 16, cy + ch - 44, 110, 26, "+ 新建用户",
             hover_btn == HB_NEWUSER, guest_mode);
    draw_btn(cx + 136, cy + ch - 44, 80, 26, "返回",
             hover_btn == HB_BACK, false);
}

static void draw_field(int32_t x, int32_t y, int32_t w, int32_t h,
                       const char *label, const char *text, int len,
                       bool password, bool focused) {
    char shown[LOGIN_PASS_MAX + 1];
    int i;
    int maxc = password ? LOGIN_PASS_MAX : LOGIN_NAME_MAX;
    if (len > maxc) len = maxc;
    for (i = 0; i < len; i++) shown[i] = password ? '*' : text[i];
    shown[i] = 0;

    gui_text(x, y - 16, label, GUI_COL_TEXT_DIM, GUI_COL_PANEL);
    gui_fill(x, y, (uint32_t)w, (uint32_t)h, GUI_COL_PANEL_LIGHT);
    gui_frame(x, y, (uint32_t)w, (uint32_t)h,
              focused ? GUI_COL_ACCENT : GUI_COL_BORDER);
    if (i > 0)
        gui_text(x + 8, y + 6, shown, GUI_COL_TEXT, GUI_COL_PANEL_LIGHT);
    if (focused) {
        int32_t tx = x + 8 + (int32_t)gui_str_width(shown);
        if (tx < x + w - 6) gui_fill(tx, y + 6, 1, 16, GUI_COL_ACCENT);
    }
}

static void draw_create_card(void) {
    int32_t cx, cy;
    create_geom(&cx, &cy);
    draw_card_base(cx, cy, 360, 300, "创建用户");

    draw_field(cx + 24, cy + 60, 312, 28, "用户名",
               create_name, create_name_len, false, focus == 0);
    draw_field(cx + 24, cy + 120, 312, 28, "密码",
               create_pass, create_pass_len, true, focus == 1);
    draw_field(cx + 24, cy + 180, 312, 28, "确认密码",
               create_confirm, create_confirm_len, true, focus == 2);

    gui_fill(cx + 24, cy + 212, 312, 16, GUI_COL_PANEL);
    if (err_msg[0])
        gui_text(cx + 24, cy + 212, err_msg, LOGIN_ERR_COLOR, GUI_COL_PANEL);

    draw_btn(cx + 74, cy + 244, 120, 26, "创建",
             hover_btn == HB_CREATE, false);
    draw_btn(cx + 206, cy + 244, 80, 26, "取消",
             hover_btn == HB_CANCEL, false);
}

static void login_draw(void) {
    int32_t sh = (int32_t)gfx_height();
    draw_wall();

    switch (state) {
    case LS_LOCK:
        draw_lock_content(0);
        break;
    case LS_ANIM_SLIDE: {
        int32_t slide = anim_frame * (sh / LOGIN_ANIM_FRAMES);
        int32_t cx, cy;
        password_geom(&cx, &cy);
        draw_lock_content(-slide);
        if (anim_target == LS_DONE)
            cy -= slide;
        else
            cy += (LOGIN_ANIM_FRAMES - anim_frame) * 8;
        draw_password_card(cx, cy, HB_NONE);
        break;
    }
    case LS_PASSWORD: {
        int32_t cx, cy;
        password_geom(&cx, &cy);
        draw_password_card(cx, cy, (int)hover_btn);
        break;
    }
    case LS_PICKER:
        draw_picker_card();
        break;
    case LS_CREATE:
        draw_create_card();
        break;
    case LS_DONE:
    default:
        break;
    }

    draw_cursor();
}

/* ---------------- 状态切换 ---------------- */

static void reset_hover(void) {
    hover_row = -1;
    hover_btn = HB_NONE;
}

static void start_anim(int target) {
    anim_frame = 0;
    anim_target = target;
    state = LS_ANIM_SLIDE;
    reset_hover();
    gui_damage();
}

static void enter_password(void) {
    state = LS_PASSWORD;
    clear_password();
    clear_error();
    reset_hover();
    gui_damage();
}

static void enter_picker(void) {
    state = LS_PICKER;
    reset_hover();
    clear_password();
    clear_error();
    gui_damage();
}

static void enter_create(void) {
    state = LS_CREATE;
    create_name_len = 0;
    create_name[0] = 0;
    create_pass_len = 0;
    create_pass[0] = 0;
    create_confirm_len = 0;
    create_confirm[0] = 0;
    focus = 0;
    clear_error();
    reset_hover();
    gui_damage();
}

static void submit_password(void) {
    uint32_t h = fnv1a32(pass_buf, pass_len);
    if (h == db.users[cur_user].pw_hash) {
        clear_error();
        clear_password();
        start_anim(LS_DONE);
    } else {
        set_error("密码错误");
        clear_password();
        gui_damage();
    }
}

static void commit_create(void) {
    if (db.user_count >= LOGIN_MAX_USERS) {
        set_error("最多 8 个用户");
        gui_damage();
        return;
    }
    if (create_name_len < 1) {
        set_error("用户名不能为空");
        gui_damage();
        return;
    }
    if (name_taken(create_name)) {
        set_error("用户名已存在");
        gui_damage();
        return;
    }
    if (create_pass_len != create_confirm_len) {
        set_error("两次输入的密码不一致");
        gui_damage();
        return;
    }

    int idx = (int)db.user_count;
    struct login_user *u = &db.users[idx];
    memset(u, 0, sizeof(*u));
    copy_name(u->name, create_name, LOGIN_NAME_MAX);
    u->pw_hash = fnv1a32(create_pass, create_pass_len);
    u->flags = 0;
    db.user_count++;

    if (!db_save()) {
        db.user_count--;
        memset(u, 0, sizeof(*u));
        set_error("保存用户失败");
        gui_damage();
        return;
    }

    cur_user = idx;
    enter_picker();
}

/* ---------------- 鼠标 ---------------- */

static void update_hover(void) {
    int32_t nr = -1;
    int32_t nb = HB_NONE;

    if (state == LS_PICKER) {
        nr = picker_row_at();
        nb = hit_button();
    } else if (state == LS_PASSWORD || state == LS_CREATE) {
        nb = hit_button();
    }

    if (nr != hover_row) {
        int32_t x, y, w, h;
        if (picker_row_rect((int)hover_row, &x, &y, &w, &h))
            gui_damage_rect(x, y, (uint32_t)w, (uint32_t)h);
        hover_row = nr;
        if (picker_row_rect((int)hover_row, &x, &y, &w, &h))
            gui_damage_rect(x, y, (uint32_t)w, (uint32_t)h);
    }

    if (nb != hover_btn) {
        int32_t x, y, w, h;
        if (button_rect((int)hover_btn, &x, &y, &w, &h))
            gui_damage_rect(x, y, (uint32_t)w, (uint32_t)h);
        hover_btn = nb;
        if (button_rect((int)hover_btn, &x, &y, &w, &h))
            gui_damage_rect(x, y, (uint32_t)w, (uint32_t)h);
    }
}

static void lock_click(void) {
    uint64_t now = timer_ticks();
    if (last_click_valid && now >= last_click_tick &&
        now - last_click_tick <= 45) {
        last_click_valid = false;
        start_anim(LS_PASSWORD);
        return;
    }
    last_click_tick = now;
    last_click_valid = true;
}

static void login_mouse(uint8_t buttons, int32_t dx, int32_t dy) {
    int32_t sw = (int32_t)gfx_width();
    int32_t sh = (int32_t)gfx_height();
    int32_t ox = cursor_x, oy = cursor_y;

    cursor_x += dx;
    cursor_y -= dy;                 /* PS/2 的 dy 向上为正 */
    if (cursor_x < 0) cursor_x = 0;
    if (cursor_x >= sw) cursor_x = sw - 1;
    if (cursor_y < 0) cursor_y = 0;
    if (cursor_y >= sh) cursor_y = sh - 1;

    if (cursor_x != ox || cursor_y != oy) {
        damage_cursor_at(ox, oy);
        damage_cursor_at(cursor_x, cursor_y);
    }

    update_hover();

    {
        int left = buttons & 1;
        int pressed = left && !prev_buttons;
        prev_buttons = (uint8_t)left;
        if (!pressed) return;
    }

    if (state == LS_LOCK) {
        lock_click();
        return;
    }

    if (state == LS_PASSWORD) {
        int hb = hit_button();
        if (hb == HB_SIGNIN) {
            submit_password();
        } else if (hb == HB_SWITCH) {
            if (!guest_mode) enter_picker();
        }
        return;
    }

    if (state == LS_PICKER) {
        int row = picker_row_at();
        int hb = hit_button();
        if (row >= 0) {
            cur_user = row;
            enter_password();
            return;
        }
        if (hb == HB_NEWUSER) {
            if (!guest_mode) enter_create();
            return;
        }
        if (hb == HB_BACK) {
            enter_password();
            return;
        }
        return;
    }

    if (state == LS_CREATE) {
        int32_t cx, cy;
        int hb = hit_button();
        create_geom(&cx, &cy);
        if (hb == HB_CREATE) {
            commit_create();
            return;
        }
        if (hb == HB_CANCEL) {
            enter_picker();
            return;
        }
        /* 输入框聚焦 */
        if (gui_hit(cursor_x, cursor_y, cx + 24, cy + 60, 312, 28)) {
            focus = 0; gui_damage();
        } else if (gui_hit(cursor_x, cursor_y, cx + 24, cy + 120, 312, 28)) {
            focus = 1; gui_damage();
        } else if (gui_hit(cursor_x, cursor_y, cx + 24, cy + 180, 312, 28)) {
            focus = 2; gui_damage();
        }
        return;
    }
}

/* ---------------- 键盘 ---------------- */

static void login_key(uint16_t key) {
    if (state == LS_LOCK) {
        if (key != KEY_ESC) start_anim(LS_PASSWORD);
        return;
    }

    if (state == LS_PASSWORD) {
        if (key == KEY_ESC) {
            clear_password();
            clear_error();
            state = LS_LOCK;
            reset_hover();
            gui_damage();
            return;
        }
        if (key == '\n' || key == '\r') {
            submit_password();
            return;
        }
        if (key == '\b') {
            if (pass_len > 0) {
                pass_len--;
                pass_buf[pass_len] = 0;
                clear_error();
                gui_damage();
            }
            return;
        }
        if (key >= 32 && key <= 126 && pass_len < LOGIN_PASS_MAX) {
            pass_buf[pass_len++] = (char)key;
            pass_buf[pass_len] = 0;
            clear_error();
            gui_damage();
        }
        return;
    }

    if (state == LS_PICKER) {
        if (key == KEY_ESC || key == '\n' || key == '\r' || key == '\b') {
            enter_password();
        } else if (key == KEY_UP) {
            if (cur_user > 0) cur_user--;
            else cur_user = (int)db.user_count - 1;
            reset_hover();
            gui_damage();
        } else if (key == KEY_DOWN) {
            cur_user = (cur_user + 1) % (int)db.user_count;
            reset_hover();
            gui_damage();
        }
        return;
    }

    if (state == LS_CREATE) {
        if (key == KEY_ESC) {
            enter_picker();
            return;
        }
        if (key == '\t' || key == KEY_DOWN) {
            focus = (focus + 1) % 3;
            gui_damage();
            return;
        }
        if (key == KEY_UP) {
            focus = (focus + 2) % 3;
            gui_damage();
            return;
        }
        if (key == '\n' || key == '\r') {
            commit_create();
            return;
        }
        if (key == '\b') {
            if (focus == 0) {
                if (create_name_len > 0) create_name[--create_name_len] = 0;
            } else if (focus == 1) {
                if (create_pass_len > 0) create_pass[--create_pass_len] = 0;
            } else {
                if (create_confirm_len > 0)
                    create_confirm[--create_confirm_len] = 0;
            }
            clear_error();
            gui_damage();
            return;
        }
        if (key >= 32 && key <= 126) {
            if (focus == 0) {
                bool ok = (key >= 'a' && key <= 'z') ||
                          (key >= 'A' && key <= 'Z') ||
                          (key >= '0' && key <= '9') ||
                          key == '_' || key == '-';
                if (ok && create_name_len < LOGIN_NAME_MAX) {
                    create_name[create_name_len++] = (char)key;
                    create_name[create_name_len] = 0;
                    clear_error();
                    gui_damage();
                }
            } else if (focus == 1) {
                if (create_pass_len < LOGIN_PASS_MAX) {
                    create_pass[create_pass_len++] = (char)key;
                    create_pass[create_pass_len] = 0;
                    clear_error();
                    gui_damage();
                }
            } else {
                if (create_confirm_len < LOGIN_PASS_MAX) {
                    create_confirm[create_confirm_len++] = (char)key;
                    create_confirm[create_confirm_len] = 0;
                    clear_error();
                    gui_damage();
                }
            }
        }
        return;
    }
}

/* ---------------- tick ---------------- */

static void login_tick(uint64_t ticks) {
    if (state == LS_ANIM_SLIDE) {
        if (anim_frame < LOGIN_ANIM_FRAMES) anim_frame++;
        if (anim_frame >= LOGIN_ANIM_FRAMES) {
            state = anim_target;
            if (state == LS_PASSWORD) {
                clear_password();
                clear_error();
                reset_hover();
            }
        }
        gui_damage();
        return;
    }

    if (state == LS_LOCK && (ticks % 100) == 0) {
        refresh_clock();
        gui_damage();
        return;
    }

    /* 静态页面没有变化时只标 1x1 脏区，避免 desktop_tick 每 tick
       因 !dmg_able 而整屏合成。 */
    gui_damage_rect(0, 0, 1, 1);
}

/* ---------------- 入口 ---------------- */

void login_set_post_login_hook(void (*fn)(void)) {
    post_login_hook = fn;
}

void login_run(void) {
    db_load();
    state = LS_LOCK;
    cur_user = 0;
    clear_password();
    clear_error();
    reset_hover();
    anim_frame = 0;
    anim_target = LS_LOCK;
    last_click_valid = false;
    last_click_tick = 0;
    prev_buttons = 0;

    refresh_clock();

    /* 把 GUI 桌面指针停到左上角；登录界面自己画一个跟手的指针。 */
    gui_mouse_event(0, -((int32_t)gfx_width() + 16),
                    (int32_t)gfx_height() + 16);
    cursor_x = (int32_t)gfx_width() / 2;
    cursor_y = (int32_t)gfx_height() / 2;

    gui_set_login_hooks(login_draw, login_mouse, login_key, login_tick);
    gui_damage();
    gui_repaint();                 /* 中断还没开，先画第一帧 */

    __asm__ volatile ("sti");

    while (state != LS_DONE) {
        struct input_event ev;

        /* 输入子系统统一取事件：键盘事件走 gui_key_event（分流到 login_key），
           鼠标事件走 gui_mouse_event（分流到 login_mouse）。 */
        input_pump();
        while (input_poll(&ev)) {
            if (ev.type == INPUT_EV_KEY) {
                gui_key_event(ev.key);
            } else if (ev.type == INPUT_EV_MOUSE) {
                gui_mouse_event(ev.buttons, ev.dx, ev.dy);
            }
            if (state == LS_DONE) break;
        }
        if (state == LS_DONE) break;
        __asm__ volatile ("hlt");
    }

    gui_clear_login_hooks();
    /* 登录结束直接进桌面：不再自动弹出终端窗口 */
    if (post_login_hook) post_login_hook();
    gui_damage();

    __asm__ volatile ("cli");
}
