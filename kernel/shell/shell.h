#ifndef SHELL_H
#define SHELL_H

/* 执行一条命令行。会就地分词，因此会修改 cmdline 的内容。
   所有输出经 console 打印到当前终端窗口；本函数不阻塞。 */
void shell_execute(char *cmdline);

#endif
