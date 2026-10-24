include config.mk

AS      := nasm
ASFLAGS := -f elf64

# 头文件依赖跟踪（改了 .h 也会触发重编）
CFLAGS  += -MMD -MP

CSRCS := $(shell find kernel -name '*.c')
ASRCS := $(shell find kernel -name '*.asm')
COBJS := $(patsubst kernel/%.c,build/%.o,$(CSRCS))
AOBJS := $(patsubst kernel/%.asm,build/%.o,$(ASRCS))
OBJS  := $(COBJS) $(AOBJS)
DEPS  := $(COBJS:.o=.d)

# 用外部工具把 resource/logo.raw 转成内核内置兜底图片数组
# （splash.c 需要 #include 它，logo.raw 变更后自动重新生成）
LOGO := resource/logo.raw
LOGO_HDR := kernel/gfx/fontdata/logo_data.h

# SSFN 矢量字体：由 sfnconv 从 TTF 转换而来（如 sfnconv --type= siyuan.ttf siyuan.sfn）。
# 全项目只认这一个文件名，禁止在各处再写死别的 .sfn。
SFN     := kernel/gfx/fontdata/siyuan.sfn
SFN_DST := usr/share/fonts/siyuan.sfn

# 防呆：字体缺失时除 clean 外的一切目标直接报错，避免打包出没有字体的 ISO
ifeq ($(filter clean,$(MAKECMDGOALS)),)
ifeq ($(wildcard $(SFN)),)
$(error 请先运行 sfnconv 生成 $(SFN))
endif
endif

# 用户态示例程序：user/hello.asm --(nasm -f bin)--> build/hello.bin
# --(gen_userbin.py)--> kernel/user/hello_bin.h（C 数组），由 usermode.c 内嵌。
USER_ASM := user/hello.asm
USER_BIN := build/hello.bin
USER_HDR := kernel/user/hello_bin.h
PROBE_ASM := user/probe.asm
PROBE_BIN := build/probe.bin
PROBE_HDR := kernel/user/probe_bin.h

KERNEL := build/StarRingOS.elf
ISO    := build/StarRingOS.iso
DISK   := build/disk.img
DISK_MB := 64

# 注意：all 必须在 ISO 变量定义之后，否则 $(ISO) 展开为空。
# 普通 make 也要保证磁盘镜像存在，否则用户只 build ISO 后启动会看到
# “no filesystem”（没有 MBR/没有 SRFS 分区）。这里只在缺失或生成脚本/
# logo 更新时重建；make disk/run 仍强制重建一块干净盘。
all: $(ISO) $(DISK)

$(DISK): tools/mksrfs.py $(LOGO) $(SFN)
	@mkdir -p build
	python3 tools/mksrfs.py $(DISK) $(LOGO) $(SFN) $(DISK_MB)

$(LOGO_HDR): tools/gen_logo_header.py $(LOGO)
	python3 tools/gen_logo_header.py $(LOGO) $@

build/gfx/splash.o: $(LOGO_HDR)

# 用户态示例：扁平二进制 → C 数组头文件；usermode.o 依赖它
$(USER_BIN): $(USER_ASM)
	@mkdir -p $(dir $@)
	$(AS) -f bin $< -o $@

$(USER_HDR): $(USER_BIN) tools/gen_userbin.py
	python3 tools/gen_userbin.py $(USER_BIN) $@

$(PROBE_BIN): $(PROBE_ASM)
	@mkdir -p $(dir $@)
	$(AS) -f bin $< -o $@

$(PROBE_HDR): $(PROBE_BIN) tools/gen_userbin.py
	python3 tools/gen_userbin.py $(PROBE_BIN) $@

build/user/usermode.o: $(USER_HDR) $(PROBE_HDR)

build/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: kernel/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(KERNEL): $(OBJS) linker.lds
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(ISO): $(KERNEL) limine.conf $(LOGO) $(SFN)
	rm -rf build/iso_root
	mkdir -p build/iso_root/boot/limine build/iso_root/EFI/BOOT
	cp $(KERNEL) build/iso_root/boot/StarRingOS.elf
	cp limine.conf build/iso_root/boot/limine.conf
	cp limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin build/iso_root/boot/limine/
	cp limine/BOOTX64.EFI build/iso_root/EFI/BOOT/
	# 把 resource/ 整个目录打包进 ISO，供 Limine 以 module 形式加载
	mkdir -p build/iso_root/resource
	cp -R resource/. build/iso_root/resource/
	# SSFN 矢量字体（Live 环境 / 没有 SRFS 盘时可从这里取）
	mkdir -p build/iso_root/$(dir $(SFN_DST))
	cp $(SFN) build/iso_root/$(SFN_DST)
	xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin -no-emul-boot \
	    -boot-load-size 4 -boot-info-table \
	    --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part \
	    --efi-boot-image --protective-msdos-label \
	    build/iso_root -o $@
	limine/limine bios-install $@

# 用外部工具把 assets/logo.raw 注入 SRFS 镜像的 /boot/logo.raw
# （LOGO 变量见文件开头）

disk: tools/mksrfs.py $(LOGO) $(SFN)
	@mkdir -p build
	python3 tools/mksrfs.py $(DISK) $(LOGO) $(SFN) $(DISK_MB)

# 通过 ich9-ahci 挂载一块 SATA 硬盘（sata.0），交由 ahci.c 驱动
run: $(ISO) disk
	qemu-system-x86_64 -cdrom $(ISO) \
	    -device ich9-ahci,id=sata \
	    -drive id=disk0,file=$(DISK),format=raw,if=none \
	    -device ide-hd,drive=disk0,bus=sata.0 \
	    -boot d -m 512M -serial stdio

-include $(DEPS)

.PHONY: all run disk clean

clean:
	rm -rf build
