#ifndef XHCI_H
#define XHCI_H
#include <stdint.h>
#include <stdbool.h>

/* ============================================================
 *  xHCI (USB 3.x) HID 键盘/鼠标驱动
 *
 *  目标：在纯 64 位内核里用轮询方式驱动 Boot Protocol HID 设备。
 *  - 不使用 malloc / 堆：所有 DMA 结构（DCBAA、命令环、事件环、
 *    设备上下文、输入上下文和 HID 报告缓冲）都是静态数组；
 *  - 不依赖 xHCI 中断：主循环每 10ms 调用一次 xhci_poll_keyboard()，
 *    由它自己消费事件环（Event Ring），并同时分发键盘/鼠标事件；
 *  - 一次枚举会扫描所有根端口，键盘和鼠标各占一个 slot、各有一条
 *    Interrupt IN 端点与传输环，互不影响；
 *  - 定时重扫根端口，设备后插入也能自动识别；
 *  - 所有硬件轮询都有上限，超时立刻返回，不会把系统卡死。
 *
 *  集成方式：
 *      main.c : xhci_kbd_init();              // 控制器 + 键盘/鼠标枚举
 *      GUI    : xhci_poll_keyboard();         // 每个 PIT tick / 主循环一次
 *
 *  xhci_kbd_init() 在找不到 xHCI 控制器时静默返回 false，原来的
 *  PS/2 键盘鼠标路径完全不受影响。
 * ============================================================ */

/* 初始化控制器并扫描所有根端口，尝试枚举 Boot Protocol HID 键盘
   （协议 1）与 HID Boot Mouse（协议 2）。
   成功找到键盘返回 true；没有控制器/没有键盘/初始化失败返回 false。
   即使返回 false，鼠标也可能已经配置成功（用 xhci_mouse_present() 查询）。 */
bool xhci_kbd_init(void);

/* 轮询键盘 + 鼠标事件，并做低频热插拔扫描。内部按 timer_ticks()
   限速到约 10ms 一次，因此可以在 GUI 主循环里每轮都调用。 */
void xhci_poll_keyboard(void);

/* 是否已经枚举到 USB HID 键盘 / HID Boot Mouse。 */
bool xhci_kbd_present(void);
bool xhci_mouse_present(void);

/* 控制器是否已经完成初始化（不表示一定找到了设备）。 */
bool xhci_present(void);

/* 单独初始化控制器。通常由 xhci_kbd_init() 内部调用。 */
bool xhci_init(void);

#endif
