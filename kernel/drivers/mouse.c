#include "mouse.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/cpu.h"
#include "drivers/pic.h"
#include "drivers/serial.h"
#include "drivers/i8042.h"
#include "drivers/input.h"
#include "drivers/timer.h"      /* 断包超时：包内字节必须在 2 tick 内到齐 */

/* 8042 辅助设备读写：只在探测通过后使用。 */
static void mouse_write(uint8_t v) {
    i8042_wait_write();
    outb(I8042_STATUS, 0xD4);   /* 下一字节发给辅助设备 */
    i8042_wait_write();
    outb(I8042_DATA, v);
}

static uint8_t mouse_read(void) {
    if (!i8042_wait_read()) return 0;
    return inb(I8042_DATA);
}

static void mouse_handler(struct regs *r) {
    (void)r;
    static uint8_t buf[3];
    static uint8_t idx = 0;

    /* 0x60 为键盘/鼠标共用缓冲：只处理确实来自 AUX（鼠标）的字节，
       否则留给键盘，避免两条数据流互相“偷”字节而错位。 */
    uint8_t st = inb(I8042_STATUS);
    if (!(st & 0x01)) return;            /* 输出缓冲为空 */
    if (!(st & 0x20)) return;            /* 非 AUX 数据 = 键盘，不归鼠标 */

    uint8_t b = inb(I8042_DATA);

    /* 断包超时：一个 PS/2 包的 3 个字节应在 1ms 内连续到达。若上一字节
       距今超过 2 个 tick（20ms），说明中间丢了字节，半包不可信 —— 直接
       丢弃，把当前字节当作新的包首重新同步。这是丢字节后唯一的可靠判据
       （位移字节的 bit3 是数据，不能用来判包首）。 */
    static uint64_t last_tick = 0;
    uint64_t now = timer_ticks();
    if (idx > 0 && (now - last_tick) > 2) idx = 0;
    last_tick = now;

    /* 同步只在“包边界”做：
         - 包首（idx==0）必须是 bit3=1 的状态字节；
         - 0xFA/0xAA 是迟到的 ACK/自检，丢弃。
       注意：包内第 2、3 字节是纯位移数据，bit3 完全可能为 1。若在
       idx>0 时把它当新包首，触摸板每次中速移动（|dx| 的 bit3 置位）
       都会把包撕成两半，反而制造更严重的乱跳 —— 所以这里不这么判。 */
    if (idx == 0) {
        if (b == 0xFA || b == 0xAA) return;
        if (!(b & 0x08)) return;
    }

    buf[idx++] = b;
    if (idx < 3) return;
    idx = 0;

    /* 溢出位：丢字节/超高速移动时硬件会置 XOV/YOV（bit6/bit7）。
       这一包的位移已经不可信，整包丢弃，绝不用它移动光标。 */
    if (buf[0] & 0xC0) return;

    /* 9 位补码还原：符号位在状态字节 bit4/bit5，低 8 位在 buf[1]/buf[2]。
       旧代码用 (int8_t) 直接截断，丢掉了第 9 位：符号位=1 且低字节为
       0x7F 时本该是 -129，却被解成 +127 —— 这才是“光标突然反向跳一下”
       的来源之一。 */
    int32_t dx = (int32_t)buf[1] - ((buf[0] & 0x10) ? 256 : 0);
    int32_t dy = (int32_t)buf[2] - ((buf[0] & 0x20) ? 256 : 0);

    input_post_mouse(buf[0] & 0x07, dx, dy);
}

void mouse_init(void) {
    /* 物理机与虚拟机都执行 8042 初始化，但先做健康探测：
       老笔记本的 i8042 可能整条命令通道不响应，探测不过就只保留 IRQ12
       注册，绝不让启动卡在端口等待上（所有等待上限 = 1000 次）。 */
    pic_clear_mask(2);              /* 级联通道，否则 IRQ12 永远到不了 CPU */

    if (!i8042_probe()) {
        serial_puts("[SRCore] mouse: i8042 not responding, skip 8042 init\n");
    } else {
        i8042_drain();
        i8042_send_command(I8042_CMD_AUX_ON);
        i8042_drain();
        if (!i8042_update_command_byte(I8042_CTR_AUX_IRQ, I8042_CTR_AUX_CLK)) {
            serial_puts("[SRCore] mouse: cmd byte failed\n");
        } else {
            mouse_write(0xF6); mouse_read(); /* ACK：恢复默认配置 */
            mouse_write(0xF4); mouse_read(); /* ACK：开启数据流模式 */
        }
        i8042_drain();
    }

    idt_set_handler(44, mouse_handler);  /* IRQ12 -> 向量 0x2C */
    pic_clear_mask(12);
}
