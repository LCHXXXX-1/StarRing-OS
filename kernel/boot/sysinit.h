#ifndef BOOT_SYSINIT_H
#define BOOT_SYSINIT_H
#include <stdint.h>
#include <stdbool.h>

/* ============================================================
 *  开机初始化状态机（加载遮罩的后台）
 *
 *  老机器（Legacy BIOS / 双核老 i3）上 gui_init() 的第一帧很慢：
 *    - rtc_read() 在 CMOS 一直报 UIP 时会长时间自旋；
 *    - 1366x768 渐变壁纸要逐行做三次整数除法。
 *  所以把这两件耗时的事从 gui_init() 里剥出来，拆成很小的步，
 *  交给 splash_show() 的跑马灯循环逐帧执行：动画一直转，直到
 *  system_init_step() 返回 true 才结束遮罩、跳转登录界面。
 *
 *  调用顺序：gfx_init() → system_init_step()（由 splash 驱动）
 *            → gui_init()（只读这里的成品数据）
 *  纯静态内存、无浮点、无死等。
 * ============================================================ */

/* 渐变缓存行数：覆盖 1366 高的屏幕；真实高度取 min(gfx_height(), 本值) */
#define BG_CACHE_ROWS 1366u

/* 预计算好的“每行一个颜色”，下标即屏幕行号 y（0 .. BG_CACHE_ROWS-1） */
extern uint32_t bg_gradient_cache[BG_CACHE_ROWS] __attribute__((aligned(32)));

/* 已经算好的行数；bg_cache_h 变化（改分辨率）时会被清零重来 */
extern volatile uint32_t bg_cache_rows;

/* 推进一步（每次调用只做一小份工作，保证动画不掉帧）。
   全部完成返回 true；重复调用安全。 */
bool system_init_step(void);

/* 初始化是否已全部完成 */
bool system_init_done(void);

/* 遮罩动画超时兜底：强制收尾（同步补完全量预计算），绝不无限转 */
void system_init_force_done(void);

/* 当前步骤的文字提示（供开机动画显示，纯静态字符串） */
const char *system_init_label(void);

/* RTC 快照：状态机读好的时间；读取失败返回 false，调用方回退开机时长 */
bool sysinit_rtc_time(int *hour, int *min, int *sec);
bool sysinit_rtc_date(int *day, int *month, int *year);

/* 缓存对当前分辨率有效返回 true。
   若检测到分辨率与预计算时不一致（冷路径），就地同步补算一次。 */
bool bg_cache_valid(void);

#endif
