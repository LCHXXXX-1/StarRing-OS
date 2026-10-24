#ifndef SERIAL_H
#define SERIAL_H
#include <stdint.h>
#include <stdbool.h>

void serial_init(void);
/* 串口是否可用。真机上 COM1 常常被 BIOS 关闭；关闭时所有输出静默丢弃，
   绝不能让 serial_putc() 在等待 THRE 时把内核卡死。 */
bool serial_ready(void);
void serial_putc(char c);
void serial_puts(const char *s);
void serial_puthex(uint64_t v);
void serial_putdec(uint64_t v);
#endif
