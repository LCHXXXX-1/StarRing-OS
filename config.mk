CC      := gcc
LD      := ld
CFLAGS  := -g -O2 -pipe -Wall -Wextra \
           -ffreestanding -fno-stack-protector -fno-stack-check -fno-lto \
           -fno-PIC -m64 -march=x86-64 -mabi=sysv \
           -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone \
           -mcmodel=kernel \
           -Ilimine -Ikernel -Ikernel/include
LDFLAGS := -nostdlib -static -T linker.lds -z max-page-size=0x1000
