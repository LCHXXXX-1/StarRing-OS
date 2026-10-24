; user/hello.asm —— 第一个 Ring 3 程序
; 用 `nasm -f bin` 编成扁平二进制，再由 tools/gen_userbin.py 转成 C 数组，
; 内核把它原样拷贝到 0x400000 并跳过去执行。ORG 让 rel 寻址在运行期自洽。
BITS 64
ORG 0x400000

start:
    ; sys_write(fd=1, buf=msg, len=msglen)
    mov  rax, 1
    mov  rdi, 1
    lea  rsi, [rel msg]
    mov  rdx, msglen
    syscall

    ; sys_exit(0) —— 让内核继续启动（原任务样例的 jmp $ 会挂住内核，
    ; 故这里显式退回内核，既能验证 Ring3 输出，又不影响后续桌面）
    mov  rax, 60
    xor  edi, edi
    syscall

.hang:
    jmp  .hang                      ; 理论上到不了这里

msg:
    db "Hello from Ring 3!", 10
msglen equ $ - msg
