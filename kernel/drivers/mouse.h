#ifndef MOUSE_H
#define MOUSE_H
#include <stdint.h>
void mouse_init(void);   /* i8042 辅助设备 + IRQ12，事件转发给 gui */
#endif