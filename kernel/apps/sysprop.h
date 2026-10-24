#ifndef SYSPROP_H
#define SYSPROP_H

/* 系统属性窗口：桌面“此电脑 → 属性”打开。
   静态单例，重复调用只聚焦已有窗口。 */
void sysprop_open(void);

#endif
