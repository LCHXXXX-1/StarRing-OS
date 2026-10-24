#ifndef I8042_H
#define I8042_H

#include <stdint.h>
#include "arch/x86_64/cpu.h"

/* PS/2 控制器（8042）寄存器与公共时序。键盘和鼠标共用 0x60 数据口，
   初始化期间如果读到/写坏了“命令字节”，会直接导致 IRQ1/IRQ12 不再
   产生，真机上表现为进桌面后完全无法操作。 */
#define I8042_DATA     0x60
#define I8042_STATUS   0x64

#define I8042_STATUS_OBF 0x01u   /* 输出缓冲有数据 */
#define I8042_STATUS_IBF 0x02u   /* 输入缓冲忙 */
#define I8042_STATUS_AUX 0x20u   /* 输出缓冲中的数据来自鼠标 */

#define I8042_CMD_SET   0x60u    /* 下一字节写命令字节 */
#define I8042_CMD_GET   0x20u    /* 读命令字节 */
#define I8042_CMD_AUX_ON 0xA8u   /* 启用辅助设备 */
#define I8042_CMD_KBD_ON 0xAEu   /* 启用键盘接口 */

#define I8042_CTR_KBD_IRQ  0x01u /* bit0：键盘 IRQ1 */
#define I8042_CTR_AUX_IRQ  0x02u /* bit1：鼠标 IRQ12 */
#define I8042_CTR_KBD_CLK  0x10u /* bit4：置 1 会关闭键盘时钟 */
#define I8042_CTR_AUX_CLK  0x20u /* bit5：置 1 会关闭鼠标时钟 */

/* 真机 i8042 的端口访问可能因 SMI 延迟到毫秒级；100000 次累计可达数分钟，
   启动看起来就是卡死。1000 次足够正常控制器完成 IBF/OBF 翻转，超时就失败
   返回，由调用方决定跳过（见 i8042_probe）。 */
static inline int i8042_wait_write(void) {
    for (int i = 0; i < 1000; i++)
        if (!(inb(I8042_STATUS) & I8042_STATUS_IBF))
            return 1;
    return 0;
}

static inline int i8042_wait_read(void) {
    for (int i = 0; i < 1000; i++)
        if (inb(I8042_STATUS) & I8042_STATUS_OBF)
            return 1;
    return 0;
}

static inline void i8042_drain(void) {
    for (int i = 0; i < 1000; i++) {
        if (!(inb(I8042_STATUS) & I8042_STATUS_OBF))
            break;
        (void)inb(I8042_DATA);
    }
}

static inline int i8042_send_command(uint8_t cmd) {
    if (!i8042_wait_write()) return 0;
    outb(I8042_STATUS, cmd);
    return 1;
}

/* 安全读取命令字节：先排空可能迟到的 ACK/鼠标数据；命令字节 bit7
   恒为 0，因此把 0xFA/0xAA 等响应识别出来并重试。 */
static inline int i8042_read_command_byte(uint8_t *out) {
    i8042_drain();
    if (!i8042_send_command(I8042_CMD_GET)) return 0;

    for (int tries = 0; tries < 3; tries++) {
        if (!i8042_wait_read()) return 0;
        uint8_t v = inb(I8042_DATA);
        if (!(v & 0x80u)) {
            *out = v;
            return 1;
        }
    }
    return 0;
}

static inline int i8042_write_command_byte(uint8_t val) {
    if (!i8042_send_command(I8042_CMD_SET)) return 0;
    if (!i8042_wait_write()) return 0;
    outb(I8042_DATA, val);
    return 1;
}

static inline int i8042_update_command_byte(uint8_t set_bits,
                                            uint8_t clear_bits) {
    uint8_t cmd;
    if (!i8042_read_command_byte(&cmd)) return 0;
    cmd = (uint8_t)((cmd | set_bits) & (uint8_t)~clear_bits);
    return i8042_write_command_byte(cmd);
}

/* 控制器健康探测：在动命令字节之前先确认 i8042 真的在响应。
   连续 8 次读 STATUS：
     - 0xFF = 端口不存在（读回全 1，PCI 遗留）
     - 0x00 = 总线异常
     - IBF 一直为 1 = 控制器忙/不响应
   任一情况判不健康，调用方应整段跳过 8042 编程，避免启动被拖死。 */
static inline int i8042_probe(void) {
    int bad = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t st = inb(I8042_STATUS);
        if (st == 0xFF || st == 0x00) return 0;
        if (st & I8042_STATUS_IBF) bad++;
        for (volatile int k = 0; k < 100; k++) ;
    }
    return bad < 8;
}

#endif