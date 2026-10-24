#include "input.h"
#include "arch/x86_64/cpu.h"          /* inb/outb 与 irq_save/irq_restore */
#include "drivers/usb/xhci.h"         /* input_pump 轮询 xHCI HID */

#define INPUT_QUEUE_SIZE 256

static struct input_event queue[INPUT_QUEUE_SIZE];
static volatile int head = 0, tail = 0;

void input_init(void) {
    head = 0;
    tail = 0;
}

/* 入队：满则丢弃新事件（宁可丢一个按键，也不能在中断里覆盖未消费的事件）。 */
static void input_push(const struct input_event *ev) {
    uint64_t flags = irq_save();
    int next = (head + 1) % INPUT_QUEUE_SIZE;
    if (next != tail) {
        queue[head] = *ev;
        head = next;
    }
    irq_restore(flags);
}

void input_post_key(uint16_t key) {
    struct input_event ev;
    ev.type = INPUT_EV_KEY;
    ev.key = key;
    ev.buttons = 0;
    ev.dx = 0;
    ev.dy = 0;
    input_push(&ev);
}

void input_post_mouse(uint8_t buttons, int32_t dx, int32_t dy) {
    struct input_event ev;
    ev.type = INPUT_EV_MOUSE;
    ev.key = 0;
    ev.buttons = buttons;
    ev.dx = dx;
    ev.dy = dy;
    input_push(&ev);
}

bool input_poll(struct input_event *ev) {
    if (!ev) return false;
    uint64_t flags = irq_save();
    if (head == tail) {
        irq_restore(flags);
        return false;
    }
    *ev = queue[tail];
    tail = (tail + 1) % INPUT_QUEUE_SIZE;
    irq_restore(flags);
    return true;
}

bool input_pending(void) {
    return head != tail;
}

void input_pump(void) {
    /* xHCI 是轮询驱动：自己判断控制器是否就绪，未初始化时立即返回。
       物理机（没有 xHCI）上这一步是空操作。 */
    xhci_poll_keyboard();
}
