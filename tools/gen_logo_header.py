#!/usr/bin/env python3
"""把原始 logo（512x512 32bpp ARGB，little-endian）转换成内核可直接
#include 的 C 数组头文件，用作开机动画的内置兜底数据。

用法:
    python3 tools/gen_logo_header.py [assets/logo.raw] [输出头文件]

输出变量:
    static const uint8_t  assets_logo_raw[];
    static const uint64_t assets_logo_raw_len;
"""
import sys
import os

SRC = sys.argv[1] if len(sys.argv) > 1 else "assets/logo.raw"
DST = sys.argv[2] if len(sys.argv) > 2 else "kernel/gfx/fontdata/logo_data.h"

with open(SRC, "rb") as f:
    data = f.read()

if len(data) == 0 or len(data) % 4 != 0:
    sys.exit("gen_logo_header: %s 不是合法的 32bpp 像素数据" % SRC)

os.makedirs(os.path.dirname(DST), exist_ok=True)

with open(DST, "w", newline="\n") as f:
    f.write("/* 自动生成的文件，请勿手工修改。\n"
            " * 来源: %s (%d 字节)\n"
            " * 重新生成: python3 tools/gen_logo_header.py\n"
            " * 用途: 磁盘上缺少 /boot/logo.raw 时（全新空盘 / mkfs 后）\n"
            " *       由 splash_show() 把这份数组写入 SRFS 作为兜底图片。 */\n"
            % (SRC, len(data)))
    f.write("#ifndef LOGO_DATA_H\n#define LOGO_DATA_H\n\n#include <stdint.h>\n\n")
    f.write("static const uint8_t assets_logo_raw[] = {\n")
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        f.write("    " + "".join("0x%02x, " % b for b in chunk) + "\n")
    f.write("};\n\n")
    f.write("static const uint64_t assets_logo_raw_len = "
            "sizeof(assets_logo_raw);\n\n")
    f.write("#endif\n")

print("gen_logo_header: %s -> %s (%d bytes)" % (SRC, DST, len(data)))
