#include "keyboard.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/hypervisor.h"
#include "drivers/pic.h"
#include "drivers/input.h"
#include "drivers/serial.h"
#include "drivers/i8042.h"
#include "arch/x86_64/cpu.h"

#define KBD_DATA   0x60
#define KBD_STATUS 0x64

/* Scancode set 1：普通键 */
static const char keymap[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0,  ' ',
};

/* Shift 时同一 scancode 对应的符号 */
static const char shift_keymap[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,  'A','S','D','F','G','H','J','K','L',':','"','~',
    0,  '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0,  ' ',
};

#define SC_LSHIFT   0x2A
#define SC_RSHIFT   0x36
#define SC_CAPS     0x3A
#define SC_CTRL     0x1D      /* E0 前缀时表示右 Ctrl */

/* E0 前缀扩展键 */
static const uint16_t ext_keymap[128] = {
    [0x48] = KEY_UP,
    [0x50] = KEY_DOWN,
    [0x4B] = KEY_LEFT,
    [0x4D] = KEY_RIGHT,
    [0x47] = KEY_HOME,
    [0x4F] = KEY_END,
    [0x53] = KEY_DELETE,
    [0x49] = KEY_PGUP,
    [0x51] = KEY_PGDN,
};

static volatile int e0_prefix = 0;
static volatile int shift_down = 0;
static volatile int caps_lock = 0;
static volatile int ctrl_left = 0;
static volatile int ctrl_right = 0;
static volatile int ctrl_usb = 0;   /* xHCI HID 键盘的 Ctrl 修饰位 */

static void keyboard_handler(struct regs *r) {
    (void)r;

    /* 8042 的输出缓冲（0x60）为键盘/鼠标共用，必须先看状态寄存器的
       AUX 位（bit5）：若挂起的字节来自鼠标，绝不能在这里读走，否则会
       把鼠标的三字节包拆散，表现为光标乱跳。留给 IRQ12 处理即可。 */
    uint8_t st = inb(KBD_STATUS);
    if (!(st & 0x01)) return;        /* 输出缓冲为空（不应发生） */
    if ( st & 0x20)   return;        /* AUX 数据 = 鼠标，不归键盘 */

    uint8_t sc = inb(KBD_DATA);

    if (sc == 0xE0) { e0_prefix = 1; return; }

    int released = sc & 0x80;
    uint8_t code = sc & 0x7F;

    if (e0_prefix) {
        e0_prefix = 0;
        if (code == SC_CTRL) { ctrl_right = released ? 0 : 1; return; }
        if (!released && ext_keymap[code]) input_post_key(ext_keymap[code]);
        return;
    }

    if (released) {
        if (code == SC_LSHIFT || code == SC_RSHIFT) shift_down = 0;
        if (code == SC_CTRL) ctrl_left = 0;
        return;
    }

    if (code == SC_LSHIFT || code == SC_RSHIFT) { shift_down = 1; return; }
    if (code == SC_CTRL) { ctrl_left = 1; return; }
    if (code == SC_CAPS) { caps_lock = !caps_lock; return; }

    char c = keymap[code];
    if (!c) return;

    /* 字母：Shift 与 Caps Lock 异或为大写；其余符号仅在按住 Shift 时替换 */
    if (c >= 'a' && c <= 'z') {
        if (shift_down ^ caps_lock) c = c - 'a' + 'A';
    } else if (shift_down && shift_keymap[code]) {
        c = shift_keymap[code];
    }

    input_post_key((uint16_t)c);
}

void keyboard_init(void) {
    /* 虚拟机里 i8042 是纳秒级，做完整初始化，保证 PS/2 键盘一定可用。
       实体机（Acer Aspire / HM55 这一代）的端口访问可能被 SMI 拖到毫秒级，
       命令字节读改写会把启动卡在 08；直接跳过，只装 IRQ1 —— 固件通常已
       启用键盘控制器，扫描码仍能进来；实在不行还有 USB 键盘（若有 xHCI）。 */
    if (is_hypervisor()) {
        i8042_drain();
        i8042_send_command(I8042_CMD_KBD_ON);
        i8042_drain();
        if (!i8042_update_command_byte(I8042_CTR_KBD_IRQ, I8042_CTR_KBD_CLK))
            serial_puts("[SRCore] kbd: cmd byte failed\n");
    } else {
        serial_puts("[SRCore] kbd: bare metal, skip i8042 init\n");
    }

    idt_set_handler(33, keyboard_handler);
    pic_clear_mask(1);
}

void keyboard_set_usb_ctrl(bool down) {
    ctrl_usb = down ? 1 : 0;
}

bool keyboard_ctrl_down(void) {
    return (ctrl_left || ctrl_right || ctrl_usb) ? true : false;
}
