#ifndef KEYBOARD_H
#define KEYBOARD_H
#include <stdint.h>
#include <stdbool.h>

/* 扩展按键（通过 input 事件队列传递，key 字段 >= 0x100） */
#define KEY_UP      0x100
#define KEY_DOWN    0x101
#define KEY_LEFT    0x102
#define KEY_RIGHT   0x103
#define KEY_HOME    0x104
#define KEY_END     0x105
#define KEY_DELETE  0x106
#define KEY_PGUP    0x107
#define KEY_PGDN    0x108
#define KEY_ESC     0x1B

void     keyboard_init(void);
bool     keyboard_ctrl_down(void); /* 左/右 Ctrl 当前是否按下（鼠标多选） */

/* xHCI HID 键盘的修饰键状态（按键事件统一走 input_post_key）。 */
void     keyboard_set_usb_ctrl(bool down);
#endif
