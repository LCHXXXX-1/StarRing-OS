#include "lib/delay.h"

/* pause 提示让超线程/低功耗核省点力，同时保证循环不被优化掉。 */
void busy_wait(uint32_t loops) {
    for (volatile uint32_t i = 0; i < loops; i++)
        __asm__ volatile ("pause");
}
