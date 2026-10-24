#ifndef RTC_H
#define RTC_H
#include <stdint.h>
#include <stdbool.h>

/* CMOS 实时钟。读失败时返回 false，调用方应回退到开机时长。
   所有等待都有次数上限（见 rtc.c），老主板 CMOS 报忙不会死锁。 */
bool rtc_read(int *hour, int *min, int *sec);
bool rtc_read_date(int *day, int *month, int *year);

/* CMOS 已被判定不可用（连续超时闩锁）；上层可停止周期性读取 */
bool rtc_dead_locked(void);

#endif
