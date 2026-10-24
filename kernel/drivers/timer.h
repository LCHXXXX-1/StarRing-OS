#ifndef TIMER_H
#define TIMER_H
#include <stdint.h>
void     timer_init(void);           /* PIT 100Hz + IRQ0 */
uint64_t timer_ticks(void);          /* 启动以来的 tick 数 */
uint64_t timer_seconds(void);        /* 启动以来的秒数 */
/* 每 tick 回调一次（中断上下文，务必简短），用于桌面刷新时钟/光标 */
void     timer_set_hook(void (*fn)(uint64_t ticks));
#endif