#ifndef LIB_DELAY_H
#define LIB_DELAY_H
#include <stdint.h>

/* 纯 CPU 忙等延时。
   用途：在没有可抢占调度、也不能依赖定时器中断的场合（开机动画在 sti 之前；
   桌面应用的长任务运行时需要立刻刷出下一帧）粗略控制帧间隔。
   loops 的量级参考：本机 100000~500000 大致对应 60FPS 的一帧，
   数值越大越慢，可按机器调整调用方传入的常量。 */
void busy_wait(uint32_t loops);

#endif
