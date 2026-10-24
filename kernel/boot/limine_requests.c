#include "limine_requests.h"

/* start / end 标记必须用 limine.h 里的官方宏，并且严格放在
   .limine_requests_start / .limine_requests_end 两个段里，
   否则 Limine 找不到请求段。

   注意：LIMINE_REQUESTS_START_MARKER / END_MARKER 是「完整声明」
   （展开形如 `uint64_t xxx[4] = {...};`），不是表达式，
   只能放在语句位置，不能塞进 `{ ... }` 初始化器里。 */

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

/* ---- 基础地址信息：AHCI / VMM 都依赖 ---- */
__attribute__((used, section(".limine_requests")))
volatile struct limine_hhdm_request g_hhdm_req = {
    .id = LIMINE_HHDM_REQUEST, .revision = 0,
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_kernel_address_request g_kaddr_req = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST, .revision = 0,
};

/* ---- 物理内存布局：PMM / meminfo 依赖 ---- */
__attribute__((used, section(".limine_requests")))
volatile struct limine_memmap_request g_memmap_req = {
    .id = LIMINE_MEMMAP_REQUEST, .revision = 0,
};

/* ---- 帧缓冲：gfx / gui 依赖 ---- */
__attribute__((used, section(".limine_requests")))
volatile struct limine_framebuffer_request g_fb_req = {
    .id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0,
};

/* ---- 内存模块：logo.raw 与 siyuan.sfn 都由 Limine 载入 ----
   全项目只允许这一份 module 请求；splash.c / font_ttf.c 通过 extern 取用。 */
__attribute__((used, section(".limine_requests")))
volatile struct limine_module_request g_module_req = {
    .id = LIMINE_MODULE_REQUEST, .revision = 0,
};

/* ---- 内核启动栈：Limine 默认只有 64 KiB ----
   SSFN 渲染/中文字形解码的调用链较深，64 KiB 会被打穿并把 .bss 里的
   cr3 / hhdm / heap_brk 一起踩坏（表现为 TTF 加载后任意 vmm_map 缺页）。
   这里请求 1 MiB，与 ssfn_render 去掉 64 KiB 栈帧的改动双保险。 */
__attribute__((used, section(".limine_requests")))
volatile struct limine_stack_size_request g_stack_req = {
    .id = LIMINE_STACK_SIZE_REQUEST, .revision = 0,
    .stack_size = 1024u * 1024u,
};

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;