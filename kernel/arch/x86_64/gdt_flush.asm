section .text

extern gdt_ptr

; void gdt_flush(void)
;   1) lgdt 新表
;   2) 重载 DS/ES/FS/GS 为内核数据段
;   3) 用 retfq 重载 CS 为内核代码段（0x08）
;   4) 重载 SS
; 必须在关中断、且尚未发生任何中断时调用。
global gdt_flush
gdt_flush:
    lgdt [rel gdt_ptr]

    mov ax, 0x10                 ; SEL_KERNEL_DATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push qword 0x08              ; 新 CS
    lea rax, [rel .reload_cs]
    push rax                     ; 新 RIP
    retfq                        ; 弹出 RIP 然后 CS -> 完成 CS 重载
.reload_cs:
    mov ax, 0x10
    mov ss, ax                   ; 64 位下 SS base/limit 被忽略，只取选择子
    ret
