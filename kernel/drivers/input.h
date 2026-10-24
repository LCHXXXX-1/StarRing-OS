#ifndef INPUT_H
#define INPUT_H
#include <stdint.h>
#include <stdbool.h>

/* ============================================================
 *  输入子系统（Phase 1）
 *
 *  所有输入驱动（PS/2 键盘/鼠标、xHCI HID）只做一件事：把规范化后的
 *  事件 input_post_*() 投进同一个队列。GUI / 登录界面只从队列取事件，
 *  不再直接调用 gui_mouse_event()，也不再关心设备来自哪个控制器。
 *
 *  生产者在中断上下文（PS/2）与主循环上下文（xHCI 轮询）都会调用投递，
 *  入队/出队都在短临界区（irq_save/irq_restore）里完成。
 * ============================================================ */

/* 事件类型 */
#define INPUT_EV_KEY     1   /* 键盘按键：key 有效 */
#define INPUT_EV_MOUSE   2   /* 鼠标：buttons/dx/dy 有效 */

struct input_event {
    uint8_t  type;       /* INPUT_EV_* */
    uint16_t key;        /* 按键码（含 KEY_* 扩展键） */
    uint8_t  buttons;    /* 鼠标按键位 */
    int32_t  dx, dy;     /* 鼠标位移（PS/2 约定：dy 向上为正） */
};

void input_init(void);

/* 驱动侧注入 */
void input_post_key(uint16_t key);
void input_post_mouse(uint8_t buttons, int32_t dx, int32_t dy);

/* 消费侧 */
bool input_poll(struct input_event *ev);
bool input_pending(void);

/* 轮询“非中断型”底层设备（xHCI 是纯轮询驱动）。
   GUI/登录循环只调这个，不再直接碰具体驱动；未初始化的驱动会立即返回。 */
void input_pump(void);

#endif
