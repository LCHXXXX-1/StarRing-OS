section .text

extern syscall_dispatch
extern syscall_log_exit
extern g_tss_rsp0

; ---- SYSCALL 入口（由 MSR_LSTAR 跳入）----
; 进入时：CPL=0，CS/SS 来自 STAR[47:32]，RIP=本函数，RCX=用户返回地址，
; R11=用户 RFLAGS，RSP 仍是用户栈（SYSCALL 不换栈！），RAX=调用号。
global syscall_entry
syscall_entry:
    swapgs                          ; 与出口配对（本内核不用 GS 局部存储，值为 0）

    ; 切到 TSS 里保存的系统调用内核栈（与 Ring3 中断共用 rsp0）
    mov [rel g_user_rsp], rsp
    mov rsp, [rel g_tss_rsp0]

    cmp rax, 60                     ; SYS_EXIT？
    je .do_exit

    ; 构造 struct syscall_regs（push 顺序与 C 结构体字段逆序对应）
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
    mov rdi, rsp                    ; 第 1 个参数 = 寄存器帧指针
    sub rsp, 8                      ; 让 call 处 16 字节对齐
    call syscall_dispatch
    add rsp, 8

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11                         ; r11 = 进入时的用户 RFLAGS
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx                         ; rcx = 用户返回 RIP
    pop rbx
    pop rax                         ; rax = 系统调用返回值

    or r11, 0x200                   ; 返回用户态时确保 IF=1

    ; 手工搭 iretq 帧：SS / RSP / RFLAGS / CS / RIP
    push qword 0x23                 ; SEL_USER_DATA
    push qword [rel g_user_rsp]     ; 用户 RSP
    push r11                        ; 用户 RFLAGS
    push qword 0x1B                 ; SEL_USER_CODE
    push rcx                        ; 用户 RIP（syscall 下一条）
    swapgs
    iretq

.do_exit:
    ; rdi 仍是用户传入的退出码（SYSCALL 不动 rdi）
    call syscall_log_exit
    mov rsp, [rel g_resume_rsp]     ; 回到 asm_enter_user 保存的内核栈
    swapgs
    ret                             ; “返回”到 asm_enter_user 的 C 调用者

; ---- 进入 Ring 3 ----
; asm_enter_user(rdi = entry, rsi = user_stack)
;   保存当前内核 rsp（= call 压入的返回地址处），切到用户栈，iretq 进 Ring3。
;   用户程序调用 SYS_EXIT 后，上面的 .do_exit 会恢复该 rsp 并 ret，
;   于是控制权干净地回到调用本函数的 C 代码。
global asm_enter_user
asm_enter_user:
    cli                             ; 过渡期间不响应中断
    mov [rel g_resume_rsp], rsp
    mov rsp, rsi
    push qword 0x23                 ; SS
    push rsi                        ; RSP（用户栈顶）
    push qword 0x202                ; RFLAGS：IF=1，bit1 恒 1
    push qword 0x1B                 ; CS
    push rdi                        ; RIP
    iretq

section .bss
align 16
g_user_rsp:    resq 1
global g_resume_rsp
g_resume_rsp:  resq 1
