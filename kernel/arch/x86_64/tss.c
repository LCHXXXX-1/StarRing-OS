#include "tss.h"
#include "drivers/serial.h"
#include "arch/x86_64/cpu.h"
#include "lib/mem.h"

/* gdt_flush.asm 里的 lgdt + 段寄存器重载 */
extern void gdt_flush(void);

/* ---- Ring3→Ring0 换栈用的内核栈（静态，无 malloc） ----
   该栈同时服务：Ring3 被中断时的 TSS.rsp0、以及 SYSCALL 入口。
   4 KiB 足够当前 sys_write / 定时器 tick 的调用深度；若后续在
   Ring3 窗口内加入更重的中断回调，可在此调大。 */
#define KSTACK_SIZE 4096
static uint8_t g_kstack[KSTACK_SIZE] __attribute__((aligned(16)));

/* GDT：7 个描述符（含 TSS 的低/高两半） */
static uint64_t g_gdt[7];

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* tss.asm 通过 `lgdt [gdt_ptr]` 读取，故必须是非 static 的全局符号 */
struct gdtr gdt_ptr;

static struct tss_entry g_tss;

/* syscall.asm 进入内核时从这里取栈顶（与 TSS.rsp0 同源）。 */
uint64_t g_tss_rsp0 = 0;

/* 普通代码/数据段：base=0、limit=0（64 位平坦模型下被忽略）。
   flags 高半字节 = G(bit3) D/B(bit2) L(bit1) AVL(bit0)：
     - 64 位代码段必须 L=1、D=0 → 0xA
     - 数据段用 D=1、G=1        → 0xC */
static void gdt_set_code_data(int idx, uint8_t access, uint8_t flags) {
    uint64_t d = 0;
    d |= (uint64_t)access << 40;
    d |= (uint64_t)(flags & 0x0F) << 52;
    g_gdt[idx] = d;
}

static void gdt_set_tss(uint64_t base, uint32_t limit) {
    uint32_t blo = (uint32_t)(base & 0xFFFFFFFFull);
    uint32_t bhi = (uint32_t)(base >> 32);

    uint64_t d0 = 0;
    d0 |= (uint64_t)(limit & 0xFFFF);
    d0 |= (uint64_t)(blo & 0xFFFFFF) << 16;
    d0 |= (uint64_t)0x89 << 40;                       /* 64 位可用 TSS，DPL0，P=1 */
    d0 |= (uint64_t)((limit >> 16) & 0xF) << 48;
    d0 |= (uint64_t)((blo >> 24) & 0xFF) << 56;
    g_gdt[5] = d0;
    g_gdt[6] = (uint64_t)bhi;
}

bool tss_init(void) {
    /* 1) TSS：rsp0 指向内核栈顶（向下增长，故取数组末尾） */
    memset(&g_tss, 0, sizeof(g_tss));
    uint64_t kstack_top = (uint64_t)&g_kstack[0] + sizeof(g_kstack);
    g_tss.rsp0 = kstack_top;
    g_tss_rsp0 = kstack_top;
    g_tss.iomap_base = sizeof(g_tss);                 /* 无 I/O 位图 */

    /* 2) GDT 描述符 */
    g_gdt[0] = 0;                                     /* null */
    gdt_set_code_data(1, 0x9A, 0xA);                  /* 内核代码：P1 DPL0 type A, L=1 G=1 */
    gdt_set_code_data(2, 0x92, 0xC);                  /* 内核数据：P1 DPL0 type 2, D=1 G=1 */
    gdt_set_code_data(3, 0xFA, 0xA);                  /* 用户代码：P1 DPL3 type A, L=1 G=1 -> 0x1B */
    gdt_set_code_data(4, 0xF2, 0xC);                  /* 用户数据：P1 DPL3 type 2, D=1 G=1 -> 0x23 */
    gdt_set_tss((uint64_t)&g_tss, sizeof(g_tss) - 1); /* TSS 选择子 0x28 */

    gdt_ptr.limit = sizeof(g_gdt) - 1;
    gdt_ptr.base  = (uint64_t)&g_gdt[0];

    /* 3) 装表 + 重载段寄存器（此刻 IF 仍为 0，切换是原子的） */
    gdt_flush();

    /* 4) ltr 装载 TSS 选择子 */
    __asm__ volatile ("ltr %0" :: "r"((uint16_t)SEL_TSS));

    serial_puts("[SRCore] gdt/tss ok (user cs=0x1b ss=0x23, tss=0x28)\n");
    return true;
}

uint64_t tss_kstack_top(void) {
    return (uint64_t)&g_kstack[0] + sizeof(g_kstack);
}
