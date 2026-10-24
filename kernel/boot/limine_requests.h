#ifndef LIMINE_REQUESTS_H
#define LIMINE_REQUESTS_H

#include <limine.h>

/* 所有 Limine 请求只在 boot/limine_requests.c 中定义一次，
   其它文件一律通过 extern 引用，禁止在头文件里实例化。 */

extern volatile struct limine_hhdm_request            g_hhdm_req;
extern volatile struct limine_kernel_address_request  g_kaddr_req;
extern volatile struct limine_memmap_request          g_memmap_req;
extern volatile struct limine_framebuffer_request     g_fb_req;
extern volatile struct limine_stack_size_request      g_stack_req;
/* 模块请求只有一份：splash 取 logo.raw，font_ttf 取 siyuan.sfn 都走它。
   重复定义会让 Limine 在加载内核前 PANIC。 */
extern volatile struct limine_module_request          g_module_req;

#endif