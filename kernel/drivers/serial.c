#include "drivers/serial.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/hypervisor.h"
#define COM1 0x3F8

/* 真机（尤其笔记本）上 COM1 往往没有接 16550，LPC 对 0x3F8 的读可能返回
   0x00。旧代码 while (!(inb(LSR) & 0x20)); 会在这里永久自旋，表现为
   Limine 交接后黑屏、连开机动画都到不了。
   这里先用回环模式探测 UART，再给每次发送加超时；没有串口时直接丢弃
   日志，系统继续启动。 */
static bool present = false;

bool serial_ready(void) { return present; }

void serial_init(void) {
    present = false;

    /* 物理机不初始化串口：老笔记本可能根本没有 COM1，或 LPC 对 0x3F8 的
       读返回看似正常的垃圾值。一旦 present 被误判为 true，后面每条日志
       都要轮询 LSR，真机上可能被拖到毫秒级，启动看起来就是卡死。
       虚拟机保留串口（QEMU -serial stdio 是主要调试通道）。 */
    if (!is_hypervisor()) return;

    /* 115200 8N1，FIFO 打开 */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);

    /* 回环自检：MCR.LOOP=1，写 THR 后应能从 RBR 原样读回。 */
    outb(COM1 + 4, 0x1E);
    outb(COM1 + 0, 0xAE);
    uint8_t v = inb(COM1 + 0);
    outb(COM1 + 4, 0x0B);

    uint8_t lsr = inb(COM1 + 5);
    /* 回环失败但 LSR 看起来像真实 UART 时仍允许输出；LSR==0x00/0xFF
       则基本可以确定没有串口。 */
    if (v == 0xAE || ((lsr & 0x20) && lsr != 0x00 && lsr != 0xFF))
        present = true;
}

void serial_putc(char c) {
    if (!present) return;

    /* 有上限的等待：UART 消失/挂死时不能让整个内核停在这里。 */
    for (uint32_t i = 0; i < 100000u; i++) {
        if (inb(COM1 + 5) & 0x20) {
            outb(COM1, (uint8_t)c);
            return;
        }
    }
    present = false;
}

void serial_puts(const char *s) { while (*s) serial_putc(*s++); }

void serial_puthex(uint64_t v) {
    serial_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        serial_putc("0123456789ABCDEF"[(v >> i) & 0xF]);
}

void serial_putdec(uint64_t v) {
    char buf[21]; int i = 0;
    if (v == 0) { serial_putc('0'); return; }
    while (v) { buf[i++] = '0' + v % 10; v /= 10; }
    while (i--) serial_putc(buf[i]);
}
