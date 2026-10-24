#ifndef GUI_H
#define GUI_H
#include <stdint.h>
#include <stdbool.h>

/* ---------------- 配色 ---------------- */
#define GUI_COL_DESKTOP_TOP   0x001B3E68
#define GUI_COL_DESKTOP_BOT   0x0007111D
#define GUI_COL_DESKTOP_MID   0x00112A4A
#define GUI_COL_TASKBAR       0x000D1826
#define GUI_COL_TASKBAR_TOP   0x00182A3D
#define GUI_COL_TASKBAR_BOT   0x000A131F
#define GUI_COL_TASKBAR_LINE  0x003E74B0
#define GUI_COL_TITLE_TOP     0x003B80D2
#define GUI_COL_TITLE_BOT     0x001F4F88
#define GUI_COL_TITLE_INA_TOP 0x002A3D54
#define GUI_COL_TITLE_INA_BOT 0x001D2B3C
#define GUI_COL_TITLE         0x002F6FBF
#define GUI_COL_TITLE_INACT   0x00384F6B
#define GUI_COL_BORDER_DARK   0x000B131C
#define GUI_COL_BORDER        0x005C8DBE
#define GUI_COL_WINDOW_BG     0x00101828
#define GUI_COL_PANEL         0x001A2B3E
#define GUI_COL_PANEL_LIGHT   0x00233A52
#define GUI_COL_TEXT          0x00E4ECF6
#define GUI_COL_TEXT_DIM      0x0091A7BD
#define GUI_COL_ACCENT        0x0059B0F5
#define GUI_COL_SELECT        0x002C6BA8
#define GUI_COL_SELECT_HOVER  0x00203F5E
#define GUI_COL_ROW_ALT       0x00132230
#define GUI_COL_BTN           0x001B3554
#define GUI_COL_BTN_HOVER     0x002E5B8C
#define GUI_COL_BTN_DISABLED  0x00122033
#define GUI_COL_MENU          0x00141F2E
#define GUI_COL_MENU_HDR      0x001D2E42
#define GUI_COL_CLOSE         0x00B23B2E
#define GUI_COL_CLOSE_HOVER   0x00E0544A
#define GUI_COL_WHITE         0x00FFFFFF

#define GUI_FONT_W   8
#define GUI_FONT_H   16
#define GUI_TITLE_H  24
#define GUI_BORDER   2
#define GUI_TASKBAR_H 38
#define GUI_MAX_WINDOWS 8

/* 窗口/任务栏/菜单用小图标 */
#define GUI_ICON_NONE    0
#define GUI_ICON_PC      1
#define GUI_ICON_TERM    2
#define GUI_ICON_FOLDER  3
#define GUI_ICON_RESTART 4
#define GUI_ICON_POWER   5
#define GUI_ICON_DISK    6
#define GUI_ICON_TASKS   7
#define GUI_ICON_MEM     8
#define GUI_ICON_USB     9

/* ---------------- 窗口 ---------------- */
struct gui_window {
    bool     used;
    int32_t  x, y;
    uint32_t w, h;
    char     title[48];
    uint8_t  icon;                 /* GUI_ICON_* */

    /* 最大化状态与还原几何 */
    bool     maximized;
    int32_t  rx, ry;
    uint32_t rw, rh;

    /* 应用回调（由应用在 gui_window_open 之后填）。
       on_click / on_hover 的 cx/cy 是相对窗口内容区左上角的坐标，
       指针不在窗口内时 on_hover 收到 (-1,-1)。dbl 表示双击。 */
    void (*draw)(struct gui_window *win);
    void (*on_key)(struct gui_window *win, uint16_t key);
    void (*on_click)(struct gui_window *win, int32_t cx, int32_t cy, bool dbl);
    void (*on_hover)(struct gui_window *win, int32_t cx, int32_t cy);
    /* 左键在内容区拖动/松开：pressed=true 表示仍在拖动，false 表示这一次
       拖动结束（坐标就是松开点）。用于资源管理器的框选。 */
    void (*on_drag)(struct gui_window *win, int32_t cx, int32_t cy,
                    bool pressed);
    void (*on_rclick)(struct gui_window *win, int32_t cx, int32_t cy);
    void (*on_resize)(struct gui_window *win);
    void (*on_close)(struct gui_window *win);

    void *data;
};

void     gui_init(void);
void     gui_run(void);                 /* 桌面事件主循环，不返回 */
void     gui_damage(void);              /* 请求重绘整屏（可在中断中调用） */
void     gui_damage_rect(int32_t x, int32_t y, uint32_t w, uint32_t h);
                                        /* 请求局部重绘（可在中断中调用） */

/* ---------------- 长任务支持 ----------------
 * 重要：on_click / on_hover / on_key 都是在 PS/2 中断上下文里被调用的
 * （mouse.c 的 IRQ1 处理函数直接调 gui_mouse_event）。中断上下文里
 * 既不能长时间阻塞，也不能安全地做合成上屏——所以耗时工作必须
 * 「投递」出去，由桌面主循环 gui_run() 在开中断的线程上下文执行。
 *
 *   gui_post_action(fn)  : 把 fn 排进队列，立即返回；主循环会尽快调用它。
 *                          队列满返回 false（调用方应回退 UI 状态）。
 *   gui_repaint()        : 立即合成并上屏一帧。只能在主循环上下文（即
 *                          fn 内部）调用，用来同步刷新进度动画。 */
bool     gui_post_action(void (*fn)(void));
void     gui_repaint(void);

/* ---------------- 空闲统计 / 慢速定时回调 ----------------
   空闲 tick 只在主循环执行 hlt 且 PIT 中断到来时累加；任务管理器用
   (total - idle) / total 估算 CPU 占用率。
   gui_set_slow_tick(fn)：fn 每 50 tick（约 500ms）在定时器 IRQ 上调用一次，
   只应更新静态状态并调用 gui_damage_rect()；传 NULL 注销。 */
uint64_t gui_idle_ticks(void);
void     gui_set_slow_tick(void (*fn)(uint64_t ticks));

/* ---------------- 全屏登录钩子 ----------------
   非空时 GUI 在这四处分流：鼠标事件、键盘事件、合成背景、定时 tick。
   四个钩子全为 NULL 时行为与没有这套机制时完全一致。
   登录界面是全屏 overlay，不进入窗口系统；login.c 在收到事件时只改状态
   并 gui_damage*，真正的合成为 desktop_tick 里的 composite()。 */
void     gui_set_login_hooks(
             void (*draw)(void),
             void (*on_mouse)(uint8_t buttons, int32_t dx, int32_t dy),
             void (*on_key)(uint16_t key),
             void (*on_tick)(uint64_t ticks));
void     gui_clear_login_hooks(void);

struct gui_window *gui_window_open(const char *title, int32_t x, int32_t y,
                                   uint32_t w, uint32_t h);
void     gui_window_close(struct gui_window *win);
void     gui_window_focus(struct gui_window *win);
bool     gui_window_focused(const struct gui_window *win);
void     gui_window_set_title(struct gui_window *win, const char *title);
void     gui_window_set_icon(struct gui_window *win, int icon);
void     gui_window_maximize(struct gui_window *win);   /* 最大化/还原切换 */
bool     gui_window_is_maximized(const struct gui_window *win);

/* 内容区几何（去掉边框与标题栏） */
int32_t  gui_content_x(const struct gui_window *win);
int32_t  gui_content_y(const struct gui_window *win);
uint32_t gui_content_w(const struct gui_window *win);
uint32_t gui_content_h(const struct gui_window *win);

/* ---------------- 右键上下文菜单 ----------------
   全局单例：同一时刻至多一个菜单，由 gui.c 绘制在所有窗口/任务栏之上。
     - labels: 静态字符串数组，元素为 NULL 表示一条分隔线；调用方须保证
       该数组在菜单打开期间一直有效（通常传静态 const 数组）。
     - on_pick(index, user): 选中下标 index 的项时回调，index 与 labels 下标
       一一对应（分隔线永远不会被选中）。回调运行在 PS/2 中断上下文（与
       on_click 相同），只应做状态变更 + gui_damage*，耗时工作用
       gui_post_action 投递。
     - x/y 为屏幕绝对坐标，会自动夹到屏幕内。 */
#define GUI_CTX_MAX_ITEMS 8
#define GUI_CTX_DISABLED  0x01          /* flags[i] 含此位时菜单项灰显不可点 */

/* 兼容旧接口：所有菜单项均可用。 */
void gui_context_open(int32_t x, int32_t y, const char *const *labels, int count,
                      void (*on_pick)(int index, void *user), void *user);

/* 带可用状态的版本。flags 可为 NULL；数组长度与 labels 相同，
   分隔线对应的 flags 值会被忽略。flags 数组在菜单打开期间必须有效。 */
void gui_context_open_ex(int32_t x, int32_t y, const char *const *labels,
                         const uint8_t *flags, int count,
                         void (*on_pick)(int index, void *user), void *user);
void gui_context_close(void);
bool gui_context_active(void);

/* ---------------- 绘制原语（写进 backbuffer） ---------------- */
void     gui_fill(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color);
void     gui_pixel(int32_t x, int32_t y, uint32_t color);
void     gui_frame(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color);
void     gui_text(int32_t x, int32_t y, const char *s, uint32_t fg, uint32_t bg);
void     gui_text_bold(int32_t x, int32_t y, const char *s, uint32_t fg,
                       uint32_t bg);
void     gui_text_max(int32_t x, int32_t y, const char *s, int max_chars,
                      uint32_t fg, uint32_t bg);
void     gui_vgrad(int32_t x, int32_t y, uint32_t w, uint32_t h,
                   uint32_t top, uint32_t bot);
void     gui_round_fill(int32_t x, int32_t y, uint32_t w, uint32_t h,
                        uint32_t color, uint32_t corner_bg);
uint32_t gui_str_width(const char *s);

/* TTF 字体是否可用；false 时所有文本自动回退 8x16 点阵 */
bool     is_ttf_ready(void);
bool     gui_hit(int32_t px, int32_t py, int32_t x, int32_t y,
                 uint32_t w, uint32_t h);

/* ---------------- 图标 ---------------- */
void     gui_icon_pc(int32_t x, int32_t y);
void     gui_icon_term(int32_t x, int32_t y);
void     gui_icon_folder(int32_t x, int32_t y);
void     gui_icon_file(int32_t x, int32_t y);
void     gui_icon_disk(int32_t x, int32_t y);
void     gui_icon_tasks(int32_t x, int32_t y);
void     gui_icon_mem(int32_t x, int32_t y);
/* 16x16 小图标，用于任务栏与开始菜单 */
void     gui_draw_mini_icon(int32_t x, int32_t y, int icon);

/* 由鼠标驱动 / 主循环调用 */
void     gui_mouse_event(uint8_t buttons, int32_t dx, int32_t dy);
void     gui_key_event(uint16_t key);

#endif
