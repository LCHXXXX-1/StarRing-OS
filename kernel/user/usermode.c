#include "arch/x86_64/syscall.h"
#include "arch/x86_64/tss.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "drivers/serial.h"
#include "gfx/console.h"
#include "hello_bin.h"
#include "probe_bin.h"

/* ============================================================
 *  用户态引导：建页 → 拷机器码 → 进入 Ring3 → （SYS_EXIT/异常后）退回内核
 *
 *  虚拟地址布局：
 *      0x400000  代码页  USER | PRESENT（只读、可执行，W=0 NX=0）
 *      0x401000  栈  页  USER | PRESENT | WRITE | NX
 *  用户栈顶给到 0x402000（向下增长）。
 *
 *  安全步骤：
 *   1) vmm_clear_user()：全地址空间 U 位清零，内核/HHDM 全部变为
 *      supervisor-only；随后只有这两页重新带 USER。
 *   2) 代码页先以 WRITE 映射把机器码拷进去，再重映射成只读可执行，
 *      用户态无法改写自己的代码，也无法执行栈（栈页带 NX）。
 *   3) sys_write 校验缓冲区必须落在用户窗口内。
 *   4) 任何来自 CPL3 的 #PF/#GP/#UD 都由 syscall_impl.c 的兜底处理
 *      安全退回内核，不会带崩系统。
 * ============================================================ */

#define USER_CODE_VA   0x400000ull
#define USER_STACK_VA  0x401000ull
#define USER_STACK_TOP 0x402000ull

/* 代码页：可读、可执行（NX=0）、不可写；栈页：可读写、不可执行（NX=1）。 */
#define USER_CODE_FLAGS  (VMM_FLAG_PRESENT | VMM_FLAG_USER)
#define USER_STACK_FLAGS (VMM_FLAG_PRESENT | VMM_FLAG_WRITE | \
                          VMM_FLAG_USER | VMM_FLAG_NX)

/* 把一段用户镜像跑起来。成功（含被兜底拦下的异常）返回 true。 */
static bool run_image(const uint8_t *bin, uint64_t len, const char *tag) {
    if (!pmm_ready() || !vmm_ready()) {
        serial_puts("[SRCore] usermode: pmm/vmm not ready\n");
        return false;
    }
    if (len == 0 || len > 4096) {
        serial_puts("[SRCore] usermode: image size invalid\n");
        return false;
    }

    /* 1) 加固：清掉所有现存映射的 USER 位 */
    vmm_clear_user();

    /* 2) 分配代码页与栈页 */
    uint64_t code_frame  = pmm_alloc_frame();
    uint64_t stack_frame = pmm_alloc_frame();
    if (!code_frame || !stack_frame) {
        serial_puts("[SRCore] usermode: out of memory\n");
        if (code_frame)  pmm_free_frame(code_frame);
        if (stack_frame) pmm_free_frame(stack_frame);
        return false;
    }

    /* 先可写，便于把镜像拷进去 */
    if (!vmm_map(USER_CODE_VA, code_frame,
                 VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER)) {
        serial_puts("[SRCore] usermode: map code page failed\n");
        goto fail;
    }
    if (!vmm_map(USER_STACK_VA, stack_frame, USER_STACK_FLAGS)) {
        serial_puts("[SRCore] usermode: map stack page failed\n");
        vmm_unmap(USER_CODE_VA, 0);
        goto fail;
    }

    /* 3) 写入机器码 */
    memcpy((void *)USER_CODE_VA, bin, len);

    /* 4) 收紧代码页：W=0, NX=0（用户态只读可执行） */
    if (!vmm_map(USER_CODE_VA, code_frame, USER_CODE_FLAGS)) {
        serial_puts("[SRCore] usermode: protect code page failed\n");
        vmm_unmap(USER_CODE_VA, 0);
        vmm_unmap(USER_STACK_VA, 0);
        goto fail;
    }

    /* 5) 清零用户栈，避免把内核残留数据泄露给 Ring3 */
    memset((void *)USER_STACK_VA, 0, 4096);

    serial_puts("[SRCore] usermode[");
    serial_puts(tag);
    serial_puts("]: loaded ");
    serial_putdec(len);
    serial_puts(" bytes at 0x400000, entering Ring3\n");

    /* 6) 进入 Ring3。异常或 SYS_EXIT 都会回到这里。 */
    syscall_set_user_active(true);
    asm_enter_user(USER_CODE_VA, USER_STACK_TOP);
    syscall_set_user_active(false);

    bool faulted = syscall_user_faulted();
    if (faulted)
        serial_puts("[SRCore] usermode: Ring3 program aborted by kernel (isolated)\n");
    else
        serial_puts("[SRCore] usermode: returned to kernel normally\n");

    /* 回收用户窗口（页表中间层保留，仅清叶项与物理帧） */
    vmm_unmap(USER_CODE_VA, 0);
    vmm_unmap(USER_STACK_VA, 0);
    pmm_free_frame(code_frame);
    pmm_free_frame(stack_frame);
    return true;

fail:
    pmm_free_frame(code_frame);
    pmm_free_frame(stack_frame);
    return false;
}

bool usermode_run_demo(void) {
    syscall_install_fault_recovery();

    console_puts("[usermode] entering Ring 3 ...\n");
    if (!run_image(hello_bin, hello_bin_len, "hello")) return false;
    if (syscall_user_faulted()) {
        console_puts("[usermode] unexpected fault in hello program.\n");
        return false;
    }
    console_puts("[usermode] back in Ring 0, demo complete.\n");
    return true;
}

bool usermode_run_probe(void) {
    syscall_install_fault_recovery();

    console_puts("[usermode] probing kernel memory from Ring 3 ...\n");
    if (!run_image(probe_bin, probe_bin_len, "probe")) return false;

    bool contained = syscall_user_faulted();
    if (contained)
        console_puts("[usermode] isolation OK: kernel access was blocked.\n");
    else
        console_puts("[usermode] ISOLATION FAILED: Ring3 read kernel memory!\n");
    return contained;
}
