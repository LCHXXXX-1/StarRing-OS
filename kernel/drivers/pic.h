#ifndef PIC_H
#define PIC_H
#include <stdbool.h>

void pic_remap(void);
void pic_disable(void);          /* IOAPIC 接管后整片屏蔽 8259 */
void pic_send_eoi(unsigned char irq);
void pic_clear_mask(unsigned char irq);
void pic_set_mask(unsigned char irq);
#endif
