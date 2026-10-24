#ifndef HYPERVISOR_H
#define HYPERVISOR_H
#include <stdint.h>

/* CPUID.1:ECX[31] = hypervisor present。
   QEMU/VMware/KVM 置 1；实体机为 0。
   用途：虚拟机做完整硬件初始化（i8042 / xHCI），实体机跳过已知会卡死或
   不存在的路径（老笔记本的慢 i8042、2010 年平台没有 xHCI）。 */
static inline int is_hypervisor(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                    : "a"(1), "c"(0));
    return (ecx >> 31) & 1;
}

#endif
