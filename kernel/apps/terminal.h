#ifndef TERMINAL_H
#define TERMINAL_H

/* 终端应用窗口：交互式 shell 的输入/输出都在这里。
   terminal_open() 创建或聚焦终端窗口；terminal_prompt() 打印新提示符。 */
void terminal_open(void);
/* 打开/聚焦终端并切换到 path（资源管理器“Open Terminal Here”）。 */
void terminal_open_at(const char *path);
void terminal_prompt(void);

#endif
