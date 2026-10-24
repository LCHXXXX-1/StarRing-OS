#!/usr/bin/env python3
"""把扁平二进制（nasm -f bin 产出的用户程序）转换成内核可 #include 的 C 数组头。

用法:
    python3 tools/gen_userbin.py build/hello.bin kernel/user/hello_bin.h

数组名与守卫宏根据输出文件名推导：hello_bin.h -> hello_bin / HELLO_BIN_H，
probe_bin.h -> probe_bin / PROBE_BIN_H。

输出变量:
    static const uint8_t  <name>[];
    static const uint64_t <name>_len;
"""
import sys
import os

SRC = sys.argv[1] if len(sys.argv) > 1 else "build/hello.bin"
DST = sys.argv[2] if len(sys.argv) > 2 else "kernel/user/hello_bin.h"

base = os.path.basename(DST)
name = os.path.splitext(base)[0]
guard = name.upper() + "_H"

with open(SRC, "rb") as f:
    data = f.read()

if len(data) == 0:
    sys.exit("gen_userbin: %s 为空" % SRC)
if len(data) > 4096:
    sys.exit("gen_userbin: %s 超过一页(4096)，请扩大用户代码页" % SRC)

os.makedirs(os.path.dirname(DST), exist_ok=True)

with open(DST, "w", newline="\n") as f:
    f.write("/* 自动生成的文件，请勿手工修改。\n"
            " * 来源: %s (%d 字节)\n"
            " * 重新生成: python3 tools/gen_userbin.py\n"
            " * 用途: usermode_run_image() 把这段机器码拷进 Ring3 代码页执行。 */\n"
            % (SRC, len(data)))
    f.write("#ifndef %s\n#define %s\n\n#include <stdint.h>\n\n" % (guard, guard))
    f.write("static const uint8_t %s[] = {\n" % name)
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        f.write("    " + "".join("0x%02x, " % b for b in chunk) + "\n")
    f.write("};\n\n")
    f.write("static const uint64_t %s_len = sizeof(%s);\n\n" % (name, name))
    f.write("#endif\n")

print("gen_userbin: %s -> %s (%d bytes, sym=%s)" % (SRC, DST, len(data), name))
