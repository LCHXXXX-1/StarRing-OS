#ifndef FILES_H
#define FILES_H

/* 资源管理器（“此电脑” This PC，Windows 11 风格）：
   左侧导航窗格 + 命令栏 + 地址栏面包屑 + 大图标网格 + 驱动器容量条 +
   状态栏。双击目录进入、双击文件以文本方式查看。
   传入 path 为初始目录（NULL/"" 视为根 "/"）。 */
void files_open(const char *path);
/* 文件系统内容变化后刷新所有已打开的文件管理器窗口 */
void files_refresh_all(void);

#endif
