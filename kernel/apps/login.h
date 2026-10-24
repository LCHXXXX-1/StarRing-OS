#ifndef LOGIN_H
#define LOGIN_H

/* 全屏登录/锁屏。阻塞运行（内部临时开中断），认证成功后返回，
   由 main.c 继续打开终端和桌面。 */
void login_run(void);

/* LS_DONE 时紧跟 terminal_open() 之后调用（main.c 注册 show_banner）。 */
void login_set_post_login_hook(void (*fn)(void));

#endif
