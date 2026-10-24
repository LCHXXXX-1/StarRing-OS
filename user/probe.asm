; user/probe.asm —— Ring3 越权访问探针
; 故意读取内核镜像基址 0xffffffff80000000。该地址的页表项在内核加固时
; 已清掉 U 位，因此 Ring3 读取必然触发 #PF(14)。内核的 usermode 兜底
; 处理会把这次异常“安全退回内核”，而不是让系统挂死。
; 若竟然读成功（说明隔离失效），程序会用 sys_exit(1) 明确报告失败。
BITS 64
ORG 0x400000

start:
    mov rax, 0xffffffff80000000
    mov rax, [rax]                  ; <- 期望在这里 #PF

    ; 走到这里说明越权读取成功了：退出码 1 表示隔离失败
    mov rax, 60
    mov edi, 1
    syscall

.hang:
    jmp .hang
