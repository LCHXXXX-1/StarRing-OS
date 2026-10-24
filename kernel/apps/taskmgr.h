#ifndef TASKMGR_H
#define TASKMGR_H

/* 任务管理器：列出系统任务与已打开的应用窗口，可“结束任务”。
   - 顶部：CPU 概要（核心数 / 运行时间 / tick）与内存占用条
   - 中部：PID / 名称 / 类型 / 运行时间 列表
   - 底部：[End Task] [Refresh]
   键盘：上下选择，Del/Enter 结束任务，R 刷新 */
void taskmgr_open(void);

#endif
