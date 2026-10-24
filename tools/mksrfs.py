#!/usr/bin/env python3
"""Generate a partitioned SRFS disk image for StarRing OS.

Disk layout produced here:

    LBA 0            : MBR (0x55AA signature + one 0x53 SRFS partition)
    LBA 1..2047      : reserved / zero padding
    LBA 2048..       : SRFS partition

Inside the SRFS partition (block size 4096, block numbers are partition-relative,
exactly what kernel/fs/fs.c expects after fs_mount(start_lba)):

    block 0        : reserved (boot)
    block 1        : superblock        (struct srfs_super, 104 bytes)
    block 2        : block bitmap
    block 3        : inode bitmap
    blocks 4..19   : inode table       (1024 inodes * 64 bytes = 16 blocks)
    blocks 20..    : data area         (block 20 = root directory)

File contents written into the image:

    /boot/logo.raw                   开机动画图片
    /usr/share/fonts/siyuan.sfn      SSFN 矢量字体（唯一字体文件名）
    /etc/system.srs                  font_path=/usr/share/fonts/siyuan.sfn

Usage:  python3 tools/mksrfs.py build/disk.img resource/logo.raw \
            kernel/gfx/fontdata/siyuan.sfn [size_mb]
"""
import struct
import sys

SECTOR      = 512
PART_START  = 2048                       # 1 MiB alignment
PART_TYPE   = 0x53                       # custom SRFS partition type

BLOCK       = 4096
N_INODES    = 1024
INODE_SIZE  = 64
DIRECT_N    = 10
INDIRECT_N  = BLOCK // 4                 # 1024 entries per indirect block
MAX_FILE_BLKS = DIRECT_N + INDIRECT_N + INDIRECT_N * INDIRECT_N
SUPER_BLOCK = 1
BMAP_BLOCK  = 2
IBMAP_BLOCK = 3
ITABLE_BLK  = 4
DATA_START  = ITABLE_BLK + (N_INODES * INODE_SIZE) // BLOCK   # 20
MAGIC       = 0x53465253                 # 'SRFS'
TYPE_FILE   = 1
TYPE_DIR    = 2

FONT_GUEST  = "/usr/share/fonts/siyuan.sfn"
SYSTEM_SRS  = (
    "# StarRing OS system settings (.srs)\n"
    "# font_path is the only source the kernel uses to load the SSFN font\n"
    "font_path=" + FONT_GUEST + "\n"
)


class SrfsImage:
    def __init__(self, total_sectors):
        self.total_blocks = (total_sectors - PART_START) * SECTOR // BLOCK
        self.img   = bytearray(total_sectors * SECTOR)
        self.bmap  = bytearray(BLOCK)
        self.imap  = bytearray(BLOCK)
        self.inodes = [bytearray(INODE_SIZE) for _ in range(N_INODES)]
        self.base  = PART_START * SECTOR
        # 目录树：路径 -> {"ino", "parent", "children": [(name, ino)]}
        self.dirs  = {"/": {"ino": 1, "parent": 1, "children": []}}
        # block 0..DATA_START-1 是元数据，预留；顺序分配提示避免每次从头扫描
        self._next_blk = DATA_START
        self._next_ino = 2
        for b in range(DATA_START):
            self.blk_set(b, 1)
        self.ino_set(0, 1)                   # inode 0 保留
        self.ino_set(1, 1)                   # inode 1 = 根目录

    # ---------- 底层块/位图 ----------
    def put(self, blk, data):
        off = self.base + blk * BLOCK
        self.img[off:off + len(data)] = data

    def blk_set(self, b, used):
        if used:
            self.bmap[b >> 3] |= 1 << (b & 7)
        else:
            self.bmap[b >> 3] &= ~(1 << (b & 7)) & 0xFF

    def blk_used(self, b):
        return (self.bmap[b >> 3] >> (b & 7)) & 1

    def ino_set(self, i, used):
        if used:
            self.imap[i >> 3] |= 1 << (i & 7)
        else:
            self.imap[i >> 3] &= ~(1 << (i & 7)) & 0xFF

    def ino_used(self, i):
        return (self.imap[i >> 3] >> (i & 7)) & 1

    def alloc_blk(self):
        for b in range(self._next_blk, self.total_blocks):
            if not self.blk_used(b):
                self.blk_set(b, 1)
                self._next_blk = b + 1
                return b
        raise SystemExit("no free block (disk image too small?)")

    def alloc_ino(self):
        for i in range(self._next_ino, N_INODES):
            if not self.ino_used(i):
                self.ino_set(i, 1)
                self._next_ino = i + 1
                return i
        raise SystemExit("no free inode")

    # ---------- inode ----------
    def set_inode(self, i, mode, nlink, size, direct, indirect=0, dindirect=0):
        direct = list(direct) + [0] * (DIRECT_N - len(direct))
        b  = struct.pack("<HHI", mode, nlink, size)   # mode + nlink + size
        b += struct.pack("<10I", *direct)             # 10 个直接块
        b += struct.pack("<I", indirect)              # 一级间接
        b += struct.pack("<I", 0)                     # mtime（镜像里不记）
        b += struct.pack("<I", dindirect)             # 二级间接（沿用保留字段）
        b += struct.pack("<I", 0)                     # pad
        assert len(b) == INODE_SIZE
        self.inodes[i][:] = b

    # ---------- 数据块分配（direct + single + double indirect） ----------
    def store(self, data):
        nb = (len(data) + BLOCK - 1) // BLOCK
        if nb > MAX_FILE_BLKS:
            raise SystemExit("file too large for SRFS (max %d blocks)"
                             % MAX_FILE_BLKS)

        blocks = [self.alloc_blk() for _ in range(nb)]
        for fb, b in enumerate(blocks):
            self.put(b, data[fb * BLOCK:(fb + 1) * BLOCK])

        direct = blocks[:DIRECT_N]
        indirect = 0
        dindirect = 0

        if nb > DIRECT_N:
            n1 = min(nb - DIRECT_N, INDIRECT_N)
            indirect = self.alloc_blk()
            ents = bytearray(BLOCK)
            for j in range(n1):
                struct.pack_into("<I", ents, j * 4, blocks[DIRECT_N + j])
            self.put(indirect, ents)

        if nb > DIRECT_N + INDIRECT_N:
            # 二级间接：dindirect 指向 1024 个叶子间接表，每个表再指 1024 块
            start = DIRECT_N + INDIRECT_N
            rest  = nb - start
            dindirect = self.alloc_blk()
            dents = bytearray(BLOCK)
            for t in range((rest + INDIRECT_N - 1) // INDIRECT_N):
                leaf = self.alloc_blk()
                lens = bytearray(BLOCK)
                cnt  = min(INDIRECT_N, rest - t * INDIRECT_N)
                for j in range(cnt):
                    struct.pack_into("<I", lens, j * 4,
                                     blocks[start + t * INDIRECT_N + j])
                self.put(leaf, lens)
                struct.pack_into("<I", dents, t * 4, leaf)
            self.put(dindirect, dents)

        return direct, indirect, dindirect

    # ---------- 目录树 ----------
    def ensure_dir(self, path):
        path = path.rstrip("/") or "/"
        if path in self.dirs:
            return self.dirs[path]["ino"]
        parent_path = path.rsplit("/", 1)[0] or "/"
        parent = self.ensure_dir(parent_path)
        ino = self.alloc_ino()
        self.dirs[path] = {"ino": ino, "parent": parent, "children": []}
        self.dirs[parent_path]["children"].append((ino, path.rsplit("/", 1)[-1]))
        return ino

    def add_file(self, arc, data):
        parent_path = arc.rsplit("/", 1)[0] or "/"
        parent = self.ensure_dir(parent_path)
        ino = self.alloc_ino()
        direct, indirect, dindirect = self.store(data)
        self.set_inode(ino, TYPE_FILE, 1, len(data), direct, indirect, dindirect)
        self.dirs[parent_path]["children"].append((ino, arc.rsplit("/", 1)[-1]))

    def dirent(self, ino, name):
        nb = name.encode("ascii")
        assert len(nb) < 60
        return struct.pack("<I", ino) + nb + b"\x00" * (60 - len(nb))

    # ---------- 落盘 ----------
    def finalize(self, img_path, size_mb, notes):
        # 目录块最后写：此时子项已全部登记
        for path, info in self.dirs.items():
            entries = [(info["ino"], "."), (info["parent"], "..")] + info["children"]
            data = b"".join(self.dirent(i, n) for i, n in entries)
            direct, indirect, dindirect = self.store(data)
            self.set_inode(info["ino"], TYPE_DIR, 2, len(data),
                           direct, indirect, dindirect)

        it = bytearray(N_INODES * INODE_SIZE)
        for i in range(N_INODES):
            it[i * INODE_SIZE:(i + 1) * INODE_SIZE] = self.inodes[i]
        self.put(ITABLE_BLK, it)
        self.put(BMAP_BLOCK, self.bmap)
        self.put(IBMAP_BLOCK, self.imap)

        free_blocks = sum(1 for b in range(self.total_blocks)
                          if not self.blk_used(b))
        free_inodes = sum(1 for i in range(N_INODES) if not self.ino_used(i))
        sb = struct.pack("<10I64s",
                         MAGIC, 1, BLOCK, self.total_blocks,
                         free_blocks, N_INODES, free_inodes,
                         (N_INODES * INODE_SIZE) // BLOCK, DATA_START, 1,
                         b"SRFS".ljust(64, b"\x00"))
        assert len(sb) == 104
        self.put(SUPER_BLOCK, sb)

        # ---- MBR at LBA 0 ----
        part_sectors = (self.total_blocks * BLOCK) // SECTOR
        mbr = bytearray(SECTOR)
        struct.pack_into("<I", mbr, 0x1B8, 0x53523031)   # disk signature 'SR01'
        entry = bytearray(16)
        entry[0] = 0x00                                   # not bootable
        entry[1:4] = bytes([0xFE, 0xFF, 0xFF])            # CHS start (LBA mode)
        entry[4] = PART_TYPE                              # 0x53 = SRFS
        entry[5:8] = bytes([0xFE, 0xFF, 0xFF])            # CHS end
        struct.pack_into("<I", entry, 8, PART_START)      # start LBA
        struct.pack_into("<I", entry, 12, part_sectors)   # sector count
        mbr[0x1BE:0x1BE + 16] = entry
        mbr[510] = 0x55
        mbr[511] = 0xAA
        self.img[0:SECTOR] = mbr

        with open(img_path, "wb") as fp:
            fp.write(self.img)

        print("[mksrfs] %s: %d MB, MBR partition 1 type=0x%02X start_lba=%d "
              "sectors=%d" % (img_path, size_mb, PART_TYPE, PART_START, part_sectors))
        print("[mksrfs] SRFS: %d blocks, free_blocks=%d, free_inodes=%d"
              % (self.total_blocks, free_blocks, free_inodes))
        for n in notes:
            print("[mksrfs] %s" % n)


def read_file(path):
    with open(path, "rb") as fp:
        return fp.read()


def build(img_path, logo_path, sfn_path, size_mb=64):
    total_sectors = (size_mb * 1024 * 1024) // SECTOR
    if total_sectors <= PART_START:
        raise SystemExit("image too small for an MBR + partition")

    image = SrfsImage(total_sectors)
    if image.total_blocks <= DATA_START + 16:
        raise SystemExit("partition too small")

    logo = read_file(logo_path)
    sfn  = read_file(sfn_path)

    notes = []
    image.add_file("/boot/logo.raw", logo)
    image.add_file(FONT_GUEST, sfn)
    image.add_file("/etc/system.srs", SYSTEM_SRS.encode("ascii"))
    notes.append("logo.raw=%d bytes, %s=%d bytes"
                 % (len(logo), FONT_GUEST, len(sfn)))
    notes.append("/etc/system.srs: font_path=%s" % FONT_GUEST)

    image.finalize(img_path, size_mb, notes)


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 1
    img_path  = sys.argv[1]
    logo_path = sys.argv[2]
    sfn_path  = sys.argv[3]
    size_mb   = int(sys.argv[4]) if len(sys.argv) > 4 else 64
    build(img_path, logo_path, sfn_path, size_mb)
    return 0


if __name__ == "__main__":
    sys.exit(main())