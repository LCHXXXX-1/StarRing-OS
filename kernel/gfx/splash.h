#ifndef SPLASH_H
#define SPLASH_H

/* 开机动画 = 「加载遮罩」：优先使用 Limine 以 module 形式加载到内存的
   resource/logo.raw（512x512 32bpp ARGB）；没有 module 时使用内核内置的
   assets_logo_raw。居中显示 logo 后播放跑马灯，并且每帧推进一次
   system_init_step()（RTC 读取 / 渐变壁纸预计算）；直到初始化全部完成
   才清屏退出，返回主流程去显示登录界面。
   必须在 gfx_init() 之后、gui_init() 之前调用；不依赖文件系统。 */
void splash_show(void);

#endif
