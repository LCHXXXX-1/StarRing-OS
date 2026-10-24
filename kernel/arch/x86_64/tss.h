#ifndef TSS_H
#define TSS_H
#include <stdint.h>
#include <stdbool.h>

/* ============================================================
 *  GDT + TSS —— 为 Ring 3 准备段描述符与硬件任务状态段
 *
 *  本模块会“接管” Limine 留下的 GDT：装一张我们自己的、带用户段与
 *  TSS 的描述符表，并立即重载 CS/DS/ES/FS/GS/SS，因此必须在 idt_init()
 *  之前调用 tss_init()（IDT 门里的段选择子取的是当时的 CS）。
 *
 *  GDT 布局（选择子 = 索引*8 | RPL）：
 *      0x00  0  null
 *      0x08  1  内核代码段   (DPL0, L=1)      -> SYSCALL 后的 CS
 *      0x10  2  内核数据段   (DPL0)           -> SYSCALL 后的 SS
 *      0x18  3  用户代码段   (DPL3, 0xFA)     -> 选择子 0x1B
 *      0x20  4  用户数据段   (DPL3, 0xF2)     -> 选择子 0x23
 *      0x28  5  TSS 低半      (0x89, 16 字节) -> 选择子 0x28
 *      0x30  6  TSS 高半
 *
 *  注意：sysretq 强制 SS = STAR[63:48]+8、CS = STAR[63:48]+16，即
 *  “数据段索引必须比代码段小 1”。这与上面“代码 0x1B / 数据 0x23”的
 *  排布不兼容，所以本内核用 iretq 返回用户态（可独立指定 CS/SS），
 *  而不是 sysretq。STAR[47:32] 仍按 SYSCALL 的要求设置。
 * ============================================================ */

#define SEL_KERNEL_CODE   0x08
#define SEL_KERNEL_DATA   0x10
#define SEL_USER_DATA     0x23        /* 索引 4，RPL3 */
#define SEL_USER_CODE     0x1B        /* 索引 3，RPL3 */
#define SEL_TSS           0x28

/* 64 位 TSS（104 字节）。我们只需要 rsp0（Ring3→Ring0 换栈）与 ist。 */
struct tss_entry {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

_Static_assert(sizeof(struct tss_entry) == 104, "TSS must be 104 bytes");

/* 安装 GDT、装载 TSS、重载段寄存器。成功返回 true。 */
bool tss_init(void);

/* 供中断/系统调用切换使用的内核栈顶（rsp0）。 */
uint64_t tss_kstack_top(void);

#endif
