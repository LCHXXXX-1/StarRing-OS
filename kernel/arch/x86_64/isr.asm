section .text

extern interrupt_dispatch
extern g_user_abort          ; 非 0 表示“刚才拦下了来自 Ring3 的异常”
extern g_resume_rsp          ; 进入 Ring3 前保存的内核栈指针

%macro ISR_NO_ERR 1
isr%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
isr%1:
    push qword %1
    jmp isr_common
%endmacro

ISR_NO_ERR 0
ISR_NO_ERR 1
ISR_NO_ERR 2
ISR_NO_ERR 3
ISR_NO_ERR 4
ISR_NO_ERR 5
ISR_NO_ERR 6
ISR_NO_ERR 7
ISR_ERR    8
ISR_NO_ERR 9
ISR_ERR    10
ISR_ERR    11
ISR_ERR    12
ISR_ERR    13
ISR_ERR    14
ISR_NO_ERR 15
ISR_NO_ERR 16
ISR_ERR    17
ISR_NO_ERR 18
ISR_NO_ERR 19
ISR_NO_ERR 20
ISR_ERR    21
ISR_NO_ERR 22
ISR_NO_ERR 23
ISR_NO_ERR 24
ISR_NO_ERR 25
ISR_NO_ERR 26
ISR_NO_ERR 27
ISR_NO_ERR 28
ISR_ERR    29
ISR_ERR    30
ISR_NO_ERR 31
ISR_NO_ERR 32
ISR_NO_ERR 33
ISR_NO_ERR 34
ISR_NO_ERR 35
ISR_NO_ERR 36
ISR_NO_ERR 37
ISR_NO_ERR 38
ISR_NO_ERR 39
ISR_NO_ERR 40
ISR_NO_ERR 41
ISR_NO_ERR 42
ISR_NO_ERR 43
ISR_NO_ERR 44
ISR_NO_ERR 45
ISR_NO_ERR 46
ISR_NO_ERR 47

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    cld
    mov rdi, rsp
    call interrupt_dispatch

    ; Ring3 异常兜底：处理程序已把 g_user_abort 置 1。
    ; 不再回填 iretq 帧（跨特权级改 CS 极易踩到 iretq 的段校验），
    ; 直接把栈切回“进入用户态之前”的内核栈并 ret，回到调用者。
    cmp byte [rel g_user_abort], 0
    jne .abort_to_kernel

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16
    iretq

.abort_to_kernel:
    mov byte [rel g_user_abort], 0
    mov rsp, [rel g_resume_rsp]
    sti
    ret

section .data
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 48
    dq isr %+ i
%assign i i+1
%endrep
