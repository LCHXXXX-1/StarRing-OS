#include "fs.h"
#include "vfs.h"
#include "partition.h"
#include "drivers/ahci.h"
#include "drivers/blkdev.h"
#include "drivers/pci.h"
#include "drivers/rtc.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "drivers/serial.h"

#define BLOCK_SIZE      4096u
#define SECTORS_PER_BLOCK (BLOCK_SIZE / 512u)
#define SRFS_MAGIC      0x53465253u   /* 'SRFS' */
#define N_INODES        1024u
#define INODE_SIZE      64u
#define DIRECT_N        10u
#define INDIRECT_ENTS   (BLOCK_SIZE / 4u)   /* 1024 */
#define ROOT_INO        1u

#define SUPER_BLOCK      1u
#define BLOCK_BMAP_BLOCK 2u
#define INODE_BMAP_BLOCK 3u
#define INODE_TABLE_BLK  4u

/* ---- 磁盘上的结构 ---- */
struct srfs_super {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t total_inodes;
    uint32_t free_inodes;
    uint32_t inode_table_blocks;
    uint32_t data_start;
    uint32_t root_inode;
    char     label[64];
} __attribute__((packed));

struct srfs_inode {
    uint16_t mode;                  /* FS_TYPE_FILE / FS_TYPE_DIR */
    uint16_t nlink;
    uint32_t size;
    uint32_t direct[DIRECT_N];
    uint32_t indirect;
    uint32_t mtime;                 /* 打包的 RTC 时间，0=未记录 */
    /* 二级间接块：占用原来的保留字段，旧磁盘上这里一定是 0，
       所以老镜像挂载行为完全不变；只有超过 10+1024 块的大文件才会用。 */
    uint32_t dindirect;
    uint32_t pad;
} __attribute__((packed));

struct srfs_dirent {
    uint32_t ino;                   /* 0 = 空槽 */
    char     name[FS_NAME_MAX];     /* 始终 NUL 结尾（创建时限制 ≤59） */
} __attribute__((packed));

_Static_assert(sizeof(struct srfs_inode) == INODE_SIZE, "inode size");
_Static_assert(sizeof(struct srfs_dirent) == 64, "dirent size");

/* 把 CMOS 时间压进 32 位：year-2000(6) | mon(4) | day(5) |
   hour(5) | min(6) | sec(6)。读取失败返回 0（旧 inode 也是 0）。 */
static uint32_t fs_now_stamp(void) {
    int h, m, s, d, mo, y;
    if (!rtc_read(&h, &m, &s) || !rtc_read_date(&d, &mo, &y)) return 0;
    if (y < 2000) y = 2000;
    if (y > 2063) y = 2063;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 ||
        h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59)
        return 0;
    return ((uint32_t)(y - 2000) << 26) | ((uint32_t)mo << 22) |
           ((uint32_t)d << 17) | ((uint32_t)h << 12) |
           ((uint32_t)m << 6) | (uint32_t)s;
}

/* ---- 块缓存（每卷独立 LRU + 脏回写） ---- */
#define NCACHE 16
struct buf {
    uint32_t blk;
    uint32_t state;                 /* 0 空, 1 干净, 2 脏 */
    uint32_t stamp;
    uint8_t  data[BLOCK_SIZE];
};

struct srfs_vol {
    bool     used;
    bool     mounted;
    int      bdev_idx;
    uint64_t part_lba;
    uint64_t sectors;
    bool     dirty;                 /* 自上次 FLUSH 后写过扇区 */
    uint32_t lru;
    struct srfs_super super;
    struct buf bcache[NCACHE];
};

static struct srfs_vol  g_vols[VFS_MAX_MOUNTS];
static struct srfs_vol *g_cur   = 0;   /* 当前 SRFS 操作使用的卷 */
static struct srfs_vol *g_root  = 0;   /* 挂在 "/" 的卷 */
static uint8_t          scratch[BLOCK_SIZE];  /* mkfs/truncate 用的静态暂存 */
static char             cwd[FS_PATH_MAX] = "/";

/* 为了尽量少改动原有 SRFS 内部代码，把「当前卷」的字段映射成旧名字。
   所有文件系统内部函数都通过 g_cur 访问实例状态，不再有全局单卷。 */
#define cache       (g_cur->bcache)
#define lru_tick    (g_cur->lru)
#define raw_dirty   (g_cur->dirty)
#define sb          (g_cur->super)
#define ready       (g_cur->mounted)
#define fs_start_lba (g_cur->part_lba)

/* ---- 定义在文件末尾的内部工具，这里前置声明 ---- */
static void buf_mark_dirty(struct buf *b);
static void super_mark_dirty(void);
static struct srfs_vol *vol_for_abs(const char *abs, const char **sub);
static const struct filesystem_ops srfs_ops;

/* 所有块号都是「分区内相对块号」，由块设备层加上分区起始偏移。 */
static bool vol_read_block(struct srfs_vol *v, uint32_t blk, uint8_t *dst) {
    if (!v) return false;
    return blkdev_read(v->bdev_idx, (uint64_t)blk * SECTORS_PER_BLOCK,
                       SECTORS_PER_BLOCK, dst);
}

static bool vol_write_block(struct srfs_vol *v, uint32_t blk,
                            const uint8_t *src) {
    if (!v) return false;
    if (!blkdev_write(v->bdev_idx, (uint64_t)blk * SECTORS_PER_BLOCK,
                      SECTORS_PER_BLOCK, src))
        return false;
    v->dirty = true;         /* 只要真的写过扇区，就欠设备一次 FLUSH */
    return true;
}

/* 当前卷的读写入口 */
static bool disk_read_block(uint32_t blk, uint8_t *dst) {
    return vol_read_block(g_cur, blk, dst);
}

static bool disk_write_block(uint32_t blk, const uint8_t *src) {
    return vol_write_block(g_cur, blk, src);
}

/* 在缓存中查找已缓存块（不填充），返回槽位或 -1 */
static int buf_lookup(uint32_t blk) {
    for (int i = 0; i < NCACHE; i++)
        if (cache[i].state && cache[i].blk == blk) return i;
    return -1;
}

static struct buf *buf_get(uint32_t blk) {
    int inv = -1, clean = -1, oldest_any = -1;
    uint32_t oldest = 0xFFFFFFFFu;

    int hit = buf_lookup(blk);
    if (hit >= 0) {
        cache[hit].stamp = ++lru_tick;
        return &cache[hit];
    }
    /* 未命中：优先空槽，其次最久未用的干净块，否则最久未用的脏块 */
    for (int i = 0; i < NCACHE; i++) {
        if (!cache[i].state) { inv = i; break; }
        if (cache[i].state == 1 &&
            (clean < 0 || cache[i].stamp < cache[clean].stamp))
            clean = i;
        if (cache[i].stamp < oldest) { oldest = cache[i].stamp; oldest_any = i; }
    }
    int v = (inv >= 0) ? inv : (clean >= 0 ? clean : oldest_any);
    struct buf *b = &cache[v];

    if (b->state == 2) {            /* 脏块先回写再复用 */
        if (!disk_write_block(b->blk, b->data)) return NULL;
        b->state = 1;
    }
    if (!disk_read_block(blk, b->data)) return NULL;
    b->blk = blk;
    b->state = 1;
    b->stamp = ++lru_tick;
    return b;
}

/* ---------------- 格式化 / 挂载 ---------------- */

/* mkfs 固定要写的非 inode-表 元数据块数：
   superblock + 块位图 + inode 位图 + root inode(重写表块 4) + root 目录块。
   inode 表清零的自适应块数(it_blocks)另算，两者相加即为总写块数，
   用来把“写了几块”换算成 0..100 的真实进度。 */
#define MKFS_FIXED_BLOCKS 5u

/* 把“已写块数/总块数”换算成百分比回报给调用方。
   上限钳到 99：100 这个读数留给 fs_format_bdev() 在 FLUSH 完成后报出。 */
static void mkfs_report(fs_format_progress_fn cb, void *user,
                        uint32_t done, uint32_t steps) {
    if (!cb) return;
    int pct = (steps > 0) ? (int)(done * 100u / steps) : 100;
    if (pct > 99) pct = 99;
    cb(pct, user);
}

/* 重建指定块设备上的全部元数据。cb/user 为可选进度回调。
   所有块号都是分区内相对块号；底层由 blkdev 加物理盘偏移。 */
static bool mkfs_core(int bdev_idx, fs_format_progress_fn cb, void *user) {
    const struct block_device *dev = blkdev_get(bdev_idx);
    if (!dev) return false;

    uint32_t total = (uint32_t)(dev->sector_count / SECTORS_PER_BLOCK);
    uint32_t it_blocks = (N_INODES * INODE_SIZE) / BLOCK_SIZE;   /* 16 */
    uint32_t data_start = INODE_TABLE_BLK + it_blocks;           /* 20 */
    if (total < data_start + 16) return false;

    uint32_t steps = it_blocks + MKFS_FIXED_BLOCKS;              /* 21 */
    uint32_t done  = 0;
    mkfs_report(cb, user, 0, steps);

    struct srfs_super fsb;
    memset(&fsb, 0, sizeof(fsb));
    fsb.magic = SRFS_MAGIC;
    fsb.version = 1;
    fsb.block_size = BLOCK_SIZE;
    fsb.total_blocks = total;
    fsb.free_blocks = total - data_start - 1;    /* root 占首个数据块 */
    fsb.total_inodes = N_INODES;
    fsb.free_inodes = N_INODES - 2;              /* 0 保留 + root */
    fsb.inode_table_blocks = it_blocks;
    fsb.data_start = data_start;
    fsb.root_inode = ROOT_INO;
    kstrcpy(fsb.label, "StarRing OS");

#define MKFS_WRITE(blk, data) \
    blkdev_write(bdev_idx, (uint64_t)(blk) * SECTORS_PER_BLOCK, \
                 SECTORS_PER_BLOCK, (data))

    /* 1 superblock */
    memset(scratch, 0, BLOCK_SIZE);
    memcpy(scratch, &fsb, sizeof(fsb));
    if (!MKFS_WRITE(SUPER_BLOCK, scratch)) return false;
    mkfs_report(cb, user, ++done, steps);

    /* 2 块位图：元数据区 + root 数据块 */
    memset(scratch, 0, BLOCK_SIZE);
    for (uint32_t i = 0; i <= data_start; i++)
        scratch[i >> 3] |= (uint8_t)(1u << (i & 7));
    if (!MKFS_WRITE(BLOCK_BMAP_BLOCK, scratch)) return false;
    mkfs_report(cb, user, ++done, steps);

    /* 3 inode 位图：bit0 保留、bit1 root */
    memset(scratch, 0, BLOCK_SIZE);
    scratch[0] = 0x03;
    if (!MKFS_WRITE(INODE_BMAP_BLOCK, scratch)) return false;
    mkfs_report(cb, user, ++done, steps);

    /* 4 inode 表清零 */
    memset(scratch, 0, BLOCK_SIZE);
    for (uint32_t i = 0; i < it_blocks; i++) {
        if (!MKFS_WRITE(INODE_TABLE_BLK + i, scratch)) return false;
        mkfs_report(cb, user, ++done, steps);
    }

    /* 5 root inode（表块 4 的 1 号槽；scratch 此时仍全零） */
    {
        struct srfs_inode root;
        memset(&root, 0, sizeof(root));
        root.mode = FS_TYPE_DIR;
        root.nlink = 2;
        root.size = 2 * 64;                     /* ".", ".." */
        root.direct[0] = data_start;
        memcpy(scratch + ROOT_INO * INODE_SIZE, &root, sizeof(root));
        if (!MKFS_WRITE(INODE_TABLE_BLK, scratch)) return false;
        mkfs_report(cb, user, ++done, steps);
    }

    /* 6 root 目录块：. 与 .. 都指向自己 */
    memset(scratch, 0, BLOCK_SIZE);
    {
        struct srfs_dirent de;
        memset(&de, 0, sizeof(de));
        de.ino = ROOT_INO;
        kstrcpy(de.name, ".");
        memcpy(scratch, &de, sizeof(de));
        kstrcpy(de.name, "..");
        memcpy(scratch + 64, &de, sizeof(de));
        if (!MKFS_WRITE(data_start, scratch)) return false;
        mkfs_report(cb, user, ++done, steps);
    }
#undef MKFS_WRITE
    return true;
}

/* 只对“起始块全零”的分区自动 mkfs。否则一个类型为 0x83 的 Linux
   分区（或任何已有文件系统的分区）会在开机时被静默清空。 */
static bool partition_is_blank(int bdev_idx) {
    if (!blkdev_read(bdev_idx, 0, SECTORS_PER_BLOCK, scratch)) return false;
    for (uint32_t i = 0; i < BLOCK_SIZE; i++)
        if (scratch[i]) return false;
    return true;
}

/* 分配 / 释放卷实例 */
static struct srfs_vol *vol_alloc(void) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_vols[i].used) {
            memset(&g_vols[i], 0, sizeof(g_vols[i]));
            g_vols[i].used = true;
            return &g_vols[i];
        }
    }
    return 0;
}

static void vol_release(struct srfs_vol *v) {
    if (!v) return;
    if (g_cur == v) g_cur = 0;
    if (g_root == v) g_root = 0;
    v->mounted = false;
    v->used    = false;
    for (int i = 0; i < NCACHE; i++) {
        v->bcache[i].state = 0;
        v->bcache[i].blk = 0;
        v->bcache[i].stamp = 0;
    }
    v->dirty = false;
    v->lru = 0;
}

/* SRFS 驱动挂载回调：source 是块设备名（"disk0p1"）。 */
static int srfs_mount_op(const char *source, void **ctx) {
    const struct block_device *dev = blkdev_find(source);
    if (!dev) {
        serial_puts("[SRCore] srfs: unknown block device\n");
        return FS_ERR_NOENT;
    }

    struct srfs_vol *v = vol_alloc();
    if (!v) return FS_ERR_NOSPACE;
    v->bdev_idx = blkdev_index_of(dev);
    v->part_lba = dev->start_lba;
    v->sectors  = dev->sector_count;
    v->mounted  = false;
    v->dirty    = false;
    v->lru      = 0;

    if (!vol_read_block(v, SUPER_BLOCK, scratch)) {
        serial_puts("[SRCore] srfs: failed to read superblock\n");
        vol_release(v);
        return FS_ERR_IO;
    }

    const struct srfs_super *dsb = (const struct srfs_super *)scratch;
    uint64_t disk_blocks = v->sectors / SECTORS_PER_BLOCK;
    bool ok = (dsb->magic == SRFS_MAGIC && dsb->block_size == BLOCK_SIZE &&
               (uint64_t)dsb->total_blocks <= disk_blocks &&
               dsb->data_start < dsb->total_blocks &&
               dsb->root_inode != 0 && dsb->root_inode < dsb->total_inodes);
    if (!ok) {
        if (!partition_is_blank(v->bdev_idx)) {
            serial_puts("[SRCore] srfs: non-SRFS filesystem; refusing to format\n");
            vol_release(v);
            return FS_ERR_INVAL;
        }
        serial_puts("[SRCore] srfs: blank partition, formatting...\n");
        if (!mkfs_core(v->bdev_idx, 0, 0)) {
            vol_release(v);
            return FS_ERR_IO;
        }
        if (!vol_read_block(v, SUPER_BLOCK, scratch)) {
            vol_release(v);
            return FS_ERR_IO;
        }
        dsb = (const struct srfs_super *)scratch;
        if (dsb->magic != SRFS_MAGIC) {
            vol_release(v);
            return FS_ERR_IO;
        }
    }

    memcpy(&v->super, dsb, sizeof(v->super));
    v->mounted = true;
    *ctx = v;

    serial_puts("[SRCore] srfs mounted ");
    serial_puts(dev->name);
    serial_puts(" label=");
    serial_puts(v->super.label);
    serial_puts(" blocks=");
    serial_putdec(v->super.total_blocks);
    serial_puts("\n");
    return FS_OK;
}

static void srfs_umount_op(void *ctx) {
    struct srfs_vol *v = (struct srfs_vol *)ctx;
    if (!v) return;
    g_cur = v;
    fs_sync();
    vol_release(v);
}

/* 挂载所有 SRFS/Linux 分区：第一个可挂载的放到 "/"，其余放到 /mnt/<bdev>。
   返回成功挂载的数量。 */
int fs_mount_all(void) {
    vfs_reset();
    g_root = 0;
    g_cur  = 0;
    kstrcpy(cwd, "/");

    /* 分区表可能刚被 Disk Utility 修改，重新生成块设备视图。 */
    blkdev_refresh();

    static const struct block_device *cand[BLKDEV_MAX];
    int ncand = 0;
    for (int i = 0; i < blkdev_count(); i++) {
        const struct block_device *b = blkdev_get(i);
        if (!b) continue;
        if (b->part_type != PART_TYPE_SRFS && b->part_type != PART_TYPE_LINUX)
            continue;
        cand[ncand++] = b;
    }
    if (ncand == 0) {
        serial_puts("[SRCore] mount: no SRFS/Linux partition found\n");
        return 0;
    }

    int mounted = 0;

    /* 先挑第一个 0x53；若它坏了，再尝试其余候选。 */
    for (int pass = 0; pass < 2 && !g_root; pass++) {
        for (int i = 0; i < ncand; i++) {
            if (pass == 0 && cand[i]->part_type != PART_TYPE_SRFS) continue;
            if (pass == 1 && cand[i]->part_type == PART_TYPE_SRFS) continue;
            int r = vfs_mount("/", &srfs_ops, cand[i]->name);
            if (r == FS_OK) {
                void *ctx = 0;
                vfs_route("/", &ctx, 0);
                g_root = (struct srfs_vol *)ctx;
                mounted++;
                break;
            }
            serial_puts("[SRCore] mount: root candidate ");
            serial_puts(cand[i]->name);
            serial_puts(" failed\n");
        }
    }
    if (!g_root) {
        serial_puts("[SRCore] mount: no root filesystem\n");
        return 0;
    }

    /* 其余分区挂到 /mnt/<bdev>。挂载失败（非 SRFS/损坏）不影响根卷。 */
    for (int i = 0; i < ncand; i++) {
        /* 已作为根挂载的设备不再重复挂载 */
        if (g_root && g_root->bdev_idx == blkdev_index_of(cand[i])) continue;

        char mp[FS_PATH_MAX];
        kstrcpy(mp, "/mnt/");
        size_t l = kstrlen(mp);
        size_t n = 0;
        while (cand[i]->name[n] && l + n + 1 < sizeof(mp)) {
            mp[l + n] = cand[i]->name[n];
            n++;
        }
        mp[l + n] = 0;

        if (vfs_mount(mp, &srfs_ops, cand[i]->name) == FS_OK) {
            mounted++;
            serial_puts("[SRCore] mount: ");
            serial_puts(cand[i]->name);
            serial_puts(" -> ");
            serial_puts(mp);
            serial_puts("\n");
        }
    }

    g_cur = g_root;

    /* 根卷首次格式化后补齐默认目录树。 */
    fs_bootstrap();
    fs_sync();
    g_cur = g_root;
    return mounted;
}

/* 兼容旧入口：按起始 LBA 找到块设备并重新挂载所有分区。 */
bool fs_mount(uint64_t start_lba) {
    if (!ahci_present()) {
        uint32_t abar = pci_scan();
        if (!abar) {
            serial_puts("[SRCore] AHCI controller not found\n");
            return false;
        }
        if (!ahci_init(abar)) return false;
    }
    blkdev_refresh();
    const struct block_device *b = 0;
    for (int i = 0; i < blkdev_count(); i++) {
        const struct block_device *c = blkdev_get(i);
        if (c && c->start_lba == start_lba &&
            c->part_type == PART_TYPE_SRFS) { b = c; break; }
    }
    if (!b) {
        for (int i = 0; i < blkdev_count(); i++) {
            const struct block_device *c = blkdev_get(i);
            if (c && c->start_lba == start_lba) { b = c; break; }
        }
    }
    if (!b) return false;
    (void)b;
    return fs_mount_all() > 0;
}

bool fs_ready(void) { return g_root && g_root->mounted; }

uint64_t fs_active_start_lba(void) {
    return fs_ready() ? g_root->part_lba : 0;
}

int fs_root_bdev_index(void) {
    return fs_ready() ? g_root->bdev_idx : -1;
}

bool fs_bdev_is_mounted(int bdev_idx) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (g_vols[i].used && g_vols[i].mounted &&
            g_vols[i].bdev_idx == bdev_idx)
            return true;
    return false;
}

/* ---------------- 格式化（磁盘工具调用） ----------------
   把指定 MBR 分区重新初始化为空的 SRFS（快格）。 */
int fs_format_bdev(int bdev_idx, fs_format_progress_fn cb, void *user) {
    const struct block_device *dev = blkdev_get(bdev_idx);
    if (!dev) return FS_ERR_INVAL;
    /* FAT/USB 等非 SRFS 兼容分区保持只读，拒绝任何格式化写入。 */
    if (dev->part_type != PART_TYPE_SRFS &&
        dev->part_type != PART_TYPE_LINUX)
        return FS_ERR_INVAL;

    bool active = (g_root && g_root->bdev_idx == bdev_idx);

    /* 先卸载所有卷，既避免旧脏块回写到正在重建的区域，也让格式化后能
       重新扫描/挂载。活动根卷被格式化后不会自动恢复，必须重启。 */
    vfs_reset();
    g_root = 0;
    g_cur  = 0;
    kstrcpy(cwd, "/");

    if (!mkfs_core(bdev_idx, cb, user)) {
        serial_puts("[SRCore] format FAILED (mkfs)\n");
        return FS_ERR_IO;
    }
    if (!blkdev_flush(bdev_idx)) {
        serial_puts("[SRCore] format: FLUSH CACHE EXT failed\n");
        return FS_ERR_IO;
    }
    if (cb) cb(100, user);

    serial_puts("[SRCore] format complete: ");
    serial_puts(dev->name);
    serial_puts(active ? " (active root - reboot required)\n"
                       : " (quick format)\n");

    if (!active)
        fs_mount_all();
    return FS_OK;
}

int fs_format_disk_ex(uint64_t start_lba, fs_format_progress_fn cb, void *user) {
    int idx = -1;
    for (int i = 0; i < blkdev_count(); i++) {
        const struct block_device *b = blkdev_get(i);
        if (b && b->start_lba == start_lba) {
            if (idx < 0 || b->part_type == PART_TYPE_SRFS) idx = i;
        }
    }
    if (idx < 0) {
        serial_puts("[SRCore] format: partition not found\n");
        return FS_ERR_NOENT;
    }
    return fs_format_bdev(idx, cb, user);
}

int fs_format_disk(void) {
    if (!fs_ready()) return FS_ERR_INVAL;
    return fs_format_bdev(g_root->bdev_idx, NULL, NULL);
}

/* ---------------- 默认目录树 ----------------
   首次格式化或旧镜像缺少这些目录时补齐一套标准结构，
   使文件管理器一开机就有内容；已存在的路径一律不动。 */

static void boot_dir(const char *path) {
    if (!fs_exists(path)) fs_create(path, FS_TYPE_DIR);
}

static void boot_file(const char *path, const char *text) {
    if (!fs_exists(path)) fs_write_file(path, text, kstrlen(text));
}

void fs_bootstrap(void) {
    static const char *dirs[] = {
        "/bin", "/boot", "/dev", "/etc", "/home", "/home/guest",
        "/home/guest/Desktop", "/home/guest/Documents",
        "/home/guest/Downloads", "/home/guest/Pictures",
        "/home/guest/Music",
        "/mnt", "/tmp", "/usr", "/usr/bin", "/usr/share",
        "/usr/share/fonts", "/var", "/var/log",
    };
    static const char *progs[] = {
        "cat", "cd", "clear", "cp", "df", "echo", "help",
        "ls", "mkdir", "pwd", "rm", "stat", "touch",
    };
    char path[FS_PATH_MAX];

    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        boot_dir(dirs[i]);

    boot_file("/readme.txt",
        "Welcome to StarRing OS!\n"
        "This disk is formatted with srfs.\n"
        "Open \"This PC\" on the desktop to browse it.\n");
    boot_file("/etc/motd", "StarRing OS - a small x86_64 operating system\n");
    boot_file("/etc/version", "StarRing OS 0.1.0\n");
    /* 系统配置（.srs）：字体路径的唯一来源；缺这个文件时内核回退默认值 */
    boot_file("/etc/system.srs",
        "# StarRing OS system settings\n"
        "font_path=/usr/share/fonts/siyuan.sfn\n");
    boot_file("/home/guest/welcome.txt",
        "Hello, guest.\nYour home directory is /home/guest.\n");
    boot_file("/home/guest/Documents/notes.txt",
        "StarRing OS notes\n"
        "----------------\n"
        "This file lives in your Documents folder.\n");
    boot_file("/home/guest/Desktop/readme.txt",
        "Drag files around the desktop, or browse the disk\n"
        "with \"This PC\".\n");

    for (size_t i = 0; i < sizeof(progs) / sizeof(progs[0]); i++) {
        kstrcpy(path, "/bin/");
        kstrcpy(path + kstrlen(path), progs[i]);
        boot_file(path, "StarRing OS built-in command\n");
    }
    fs_sync();
}

/* ---------------- inode 读写 ---------------- */

static bool inode_get(uint32_t ino, struct srfs_inode *out) {
    if (ino == 0 || ino >= sb.total_inodes) return false;
    uint32_t blk = INODE_TABLE_BLK + (ino * INODE_SIZE) / BLOCK_SIZE;
    uint32_t off = (ino * INODE_SIZE) % BLOCK_SIZE;
    struct buf *b = buf_get(blk);
    if (!b) return false;
    memcpy(out, b->data + off, sizeof(*out));
    return true;
}

static bool inode_put(uint32_t ino, const struct srfs_inode *ip) {
    if (ino == 0 || ino >= sb.total_inodes) return false;
    uint32_t blk = INODE_TABLE_BLK + (ino * INODE_SIZE) / BLOCK_SIZE;
    uint32_t off = (ino * INODE_SIZE) % BLOCK_SIZE;
    struct buf *b = buf_get(blk);
    if (!b) return false;
    memcpy(b->data + off, ip, sizeof(*ip));
    buf_mark_dirty(b);
    return true;
}

/* ---------------- 位图与块分配 ---------------- */

static int bitmap_set(uint32_t bmap_blk, uint32_t bit, int used) {
    struct buf *b = buf_get(bmap_blk);
    if (!b) return FS_ERR_IO;
    uint8_t mask = (uint8_t)(1u << (bit & 7));
    bool was = (b->data[bit >> 3] & mask) != 0;
    if (used) b->data[bit >> 3] |= mask;
    else      b->data[bit >> 3] &= (uint8_t)~mask;
    buf_mark_dirty(b);
    if (was != (used != 0)) {           /* 维护空闲计数 */
        if (bmap_blk == BLOCK_BMAP_BLOCK) {
            if (used) { if (sb.free_blocks) sb.free_blocks--; }
            else sb.free_blocks++;
        } else {
            if (used) { if (sb.free_inodes) sb.free_inodes--; }
            else sb.free_inodes++;
        }
        super_mark_dirty();
    }
    return FS_OK;
}

static int block_alloc(uint32_t *out) {
    struct buf *b = buf_get(BLOCK_BMAP_BLOCK);
    if (!b) return FS_ERR_IO;
    for (uint32_t i = sb.data_start; i < sb.total_blocks; i++) {
        if (!(b->data[i >> 3] & (1u << (i & 7)))) {
            int r = bitmap_set(BLOCK_BMAP_BLOCK, i, 1);
            if (r < 0) return r;
            struct buf *z = buf_get(i);     /* 新分配的块清零 */
            if (!z) return FS_ERR_IO;
            memset(z->data, 0, BLOCK_SIZE);
            buf_mark_dirty(z);
            *out = i;
            return FS_OK;
        }
    }
    return FS_ERR_NOSPACE;
}

static int inode_alloc(uint32_t *out) {
    struct buf *b = buf_get(INODE_BMAP_BLOCK);
    if (!b) return FS_ERR_IO;
    for (uint32_t i = 2; i < sb.total_inodes; i++) {
        if (!(b->data[i >> 3] & (1u << (i & 7)))) {
            int r = bitmap_set(INODE_BMAP_BLOCK, i, 1);
            if (r < 0) return r;
            struct srfs_inode ip;
            memset(&ip, 0, sizeof(ip));
            if (!inode_put(i, &ip)) return FS_ERR_IO;
            *out = i;
            return FS_OK;
        }
    }
    return FS_ERR_NOSPACE;
}

/* ---------------- 文件块映射 ---------------- */

/* 第 fb 逻辑块 → 磁盘块号；alloc=0 且未分配返回 0（洞）；错误返回负值
   三级映射：direct[10] -> indirect[1024] -> dindirect[1024][1024]
   单文件上限 = (10 + 1024 + 1024*1024) * 4096 ≈ 4 GiB，够装整只 SSFN 字体。 */
static int64_t file_block(struct srfs_inode *ip, uint32_t fb, bool alloc) {
    if (fb < DIRECT_N) {
        if (ip->direct[fb]) return ip->direct[fb];
        if (!alloc) return 0;
        uint32_t nb;
        int r = block_alloc(&nb);
        if (r < 0) return r;
        ip->direct[fb] = nb;
        return nb;
    }
    fb -= DIRECT_N;
    if (fb < INDIRECT_ENTS) {
        if (!ip->indirect) {
            if (!alloc) return 0;
            uint32_t nb;
            int r = block_alloc(&nb);
            if (r < 0) return r;
            ip->indirect = nb;
        }
        struct buf *ib = buf_get(ip->indirect);
        if (!ib) return FS_ERR_IO;
        uint32_t *ents = (uint32_t *)ib->data;
        if (ents[fb]) return ents[fb];
        if (!alloc) return 0;
        uint32_t nb;
        int r = block_alloc(&nb);
        if (r < 0) return r;
        /* block_alloc() 会改块位图，可能把 ib 挤出缓存 -> 重新取 */
        ib = buf_get(ip->indirect);
        if (!ib) return FS_ERR_IO;
        ents = (uint32_t *)ib->data;
        if (!ents[fb]) ents[fb] = nb;
        buf_mark_dirty(ib);
        return ents[fb];
    }

    /* ---- 二级间接（新增；旧磁盘这里全是 0，不影响已有文件） ---- */
    fb -= INDIRECT_ENTS;
    if (fb >= INDIRECT_ENTS * INDIRECT_ENTS) return FS_ERR_NOSPACE;

    uint32_t l1 = fb / INDIRECT_ENTS;
    uint32_t l2 = fb % INDIRECT_ENTS;

    if (!ip->dindirect) {
        if (!alloc) return 0;
        uint32_t nb;
        int r = block_alloc(&nb);
        if (r < 0) return r;
        ip->dindirect = nb;               /* block_alloc 已把新块清零 */
    }
    struct buf *db = buf_get(ip->dindirect);
    if (!db) return FS_ERR_IO;
    uint32_t *dents = (uint32_t *)db->data;
    uint32_t sub = dents[l1];

    if (!sub) {
        if (!alloc) return 0;
        uint32_t nb;
        int r = block_alloc(&nb);
        if (r < 0) return r;
        db = buf_get(ip->dindirect);      /* 同上：位图写入可能挤掉 db */
        if (!db) return FS_ERR_IO;
        dents = (uint32_t *)db->data;
        if (!dents[l1]) { dents[l1] = nb; buf_mark_dirty(db); }
        sub = dents[l1];
    }

    struct buf *ib = buf_get(sub);
    if (!ib) return FS_ERR_IO;
    uint32_t *ents = (uint32_t *)ib->data;
    if (ents[l2]) return ents[l2];
    if (!alloc) return 0;
    uint32_t nb;
    int r = block_alloc(&nb);
    if (r < 0) return r;
    ib = buf_get(sub);
    if (!ib) return FS_ERR_IO;
    ents = (uint32_t *)ib->data;
    if (!ents[l2]) ents[l2] = nb;
    buf_mark_dirty(ib);
    return ents[l2];
}

/* 释放 inode 占用的全部数据块，size 归零 */
static int inode_truncate(uint32_t ino, struct srfs_inode *ip) {
    for (uint32_t i = 0; i < DIRECT_N; i++)
        if (ip->direct[i]) {
            int r = bitmap_set(BLOCK_BMAP_BLOCK, ip->direct[i], 0);
            if (r < 0) return r;
            ip->direct[i] = 0;
        }
    if (ip->indirect) {
        struct buf *ib = buf_get(ip->indirect);
        if (!ib) return FS_ERR_IO;
        memcpy(scratch, ib->data, BLOCK_SIZE);  /* 先拷出，防缓存槽被淘汰 */
        const uint32_t *ents = (const uint32_t *)scratch;
        for (uint32_t i = 0; i < INDIRECT_ENTS; i++)
            if (ents[i]) {
                int r = bitmap_set(BLOCK_BMAP_BLOCK, ents[i], 0);
                if (r < 0) return r;
            }
        int r = bitmap_set(BLOCK_BMAP_BLOCK, ip->indirect, 0);
        if (r < 0) return r;
        ip->indirect = 0;
    }
    /* 二级间接：逐个取回叶子间接表，释放其数据块，再释放叶子表本身。
       每一轮都重新 buf_get 一级表，因为 bitmap_set() 会把缓存槽挤掉。 */
    if (ip->dindirect) {
        for (uint32_t t = 0; t < INDIRECT_ENTS; t++) {
            struct buf *db = buf_get(ip->dindirect);
            if (!db) return FS_ERR_IO;
            uint32_t sub = ((const uint32_t *)db->data)[t];
            if (!sub) continue;
            struct buf *ib = buf_get(sub);
            if (!ib) return FS_ERR_IO;
            memcpy(scratch, ib->data, BLOCK_SIZE);
            const uint32_t *ents = (const uint32_t *)scratch;
            for (uint32_t i = 0; i < INDIRECT_ENTS; i++)
                if (ents[i]) {
                    int r = bitmap_set(BLOCK_BMAP_BLOCK, ents[i], 0);
                    if (r < 0) return r;
                }
            int r = bitmap_set(BLOCK_BMAP_BLOCK, sub, 0);
            if (r < 0) return r;
        }
        int r = bitmap_set(BLOCK_BMAP_BLOCK, ip->dindirect, 0);
        if (r < 0) return r;
        ip->dindirect = 0;
    }
    ip->size = 0;
    uint32_t stamp = fs_now_stamp();
    if (stamp) ip->mtime = stamp;
    return inode_put(ino, ip) ? FS_OK : FS_ERR_IO;
}

/* 向 inode 的 off 处写入 len 字节，按需分配块并更新 size */
static int inode_write_at(uint32_t ino, struct srfs_inode *ip,
                          uint32_t off, const uint8_t *src, uint32_t len) {
    uint32_t done = 0;
    while (done < len) {
        uint32_t pos = off + done;
        uint32_t fb = pos / BLOCK_SIZE;
        uint32_t boff = pos % BLOCK_SIZE;
        uint32_t n = BLOCK_SIZE - boff;
        if (n > len - done) n = len - done;

        int64_t bno = file_block(ip, fb, true);
        if (bno < 0) return (int)bno;
        struct buf *b = buf_get((uint32_t)bno);
        if (!b) return FS_ERR_IO;
        memcpy(b->data + boff, src + done, n);
        buf_mark_dirty(b);
        done += n;
    }
    if (off + len > ip->size) ip->size = off + len;
    uint32_t stamp = fs_now_stamp();
    if (stamp) ip->mtime = stamp;
    return inode_put(ino, ip) ? FS_OK : FS_ERR_IO;
}

/* 从 inode 读出最多 count 字节，返回实际字节数（到文件尾为 0），错误为负 */
static int inode_read_at(const struct srfs_inode *ip, uint8_t *dst,
                         uint32_t count, uint32_t off) {
    if (off >= ip->size) return 0;
    if (count > ip->size - off) count = ip->size - off;
    uint32_t done = 0;
    struct srfs_inode tmp = *ip;   /* alloc=0 不改内容，但 file_block 要可变指针 */
    while (done < count) {
        uint32_t pos = off + done;
        uint32_t fb = pos / BLOCK_SIZE;
        uint32_t boff = pos % BLOCK_SIZE;
        uint32_t n = BLOCK_SIZE - boff;
        if (n > count - done) n = count - done;

        int64_t bno = file_block(&tmp, fb, false);
        if (bno < 0) return (int)bno;
        if (bno == 0) memset(dst + done, 0, n);     /* 洞读作零 */
        else {
            struct buf *b = buf_get((uint32_t)bno);
            if (!b) return FS_ERR_IO;
            memcpy(dst + done, b->data + boff, n);
        }
        done += n;
    }
    return (int)done;
}

/* ---------------- 目录操作 ---------------- */

/* 读第 idx 个目录项（要求 idx < size/64） */
static int dir_get_entry(uint32_t dir_ino, uint32_t idx,
                         struct srfs_dirent *out) {
    struct srfs_inode dip;
    if (!inode_get(dir_ino, &dip)) return FS_ERR_IO;
    if (dip.mode != FS_TYPE_DIR) return FS_ERR_NOTDIR;
    if ((idx + 1) * 64u > dip.size) return FS_ERR_NOENT;
    uint8_t tmp[64];
    int r = inode_read_at(&dip, tmp, 64, idx * 64u);
    if (r != 64) return (r < 0) ? r : FS_ERR_IO;
    memcpy(out, tmp, 64);
    return FS_OK;
}

/* 写第 idx 个目录项（idx 可等于当前条目数 = 追加；会增长目录 size） */
static int dir_set_entry(uint32_t dir_ino, uint32_t idx,
                         const struct srfs_dirent *e) {
    struct srfs_inode dip;
    if (!inode_get(dir_ino, &dip)) return FS_ERR_IO;
    if (dip.mode != FS_TYPE_DIR) return FS_ERR_NOTDIR;
    uint32_t off = idx * 64u;
    int r = inode_write_at(dir_ino, &dip, off, (const uint8_t *)e, 64);
    if (r < 0) return r;
    fs_sync();
    return FS_OK;
}

static int dir_lookup(uint32_t dir_ino, const char *name, uint32_t *out) {
    struct srfs_inode dip;
    if (!inode_get(dir_ino, &dip)) return FS_ERR_IO;
    if (dip.mode != FS_TYPE_DIR) return FS_ERR_NOTDIR;
    uint32_t count = dip.size / 64u;
    for (uint32_t i = 0; i < count; i++) {
        struct srfs_dirent de;
        int r = dir_get_entry(dir_ino, i, &de);
        if (r < 0) return r;
        if (de.ino && kstrcmp(de.name, name) == 0) {
            *out = de.ino;
            return FS_OK;
        }
    }
    return FS_ERR_NOENT;
}

/* 在目录中登记 name → child_ino；优先复用空槽，否则追加 */
static int dir_add(uint32_t dir_ino, const char *name, uint32_t child) {
    struct srfs_inode dip;
    if (!inode_get(dir_ino, &dip)) return FS_ERR_IO;
    if (dip.mode != FS_TYPE_DIR) return FS_ERR_NOTDIR;
    uint32_t count = dip.size / 64u;

    uint32_t slot = count;              /* 默认追加 */
    for (uint32_t i = 0; i < count; i++) {
        struct srfs_dirent de;
        int r = dir_get_entry(dir_ino, i, &de);
        if (r < 0) return r;
        if (de.ino == 0) { slot = i; break; }
    }
    struct srfs_dirent de;
    memset(&de, 0, sizeof(de));
    de.ino = child;
    kstrcpy(de.name, name);
    return dir_set_entry(dir_ino, slot, &de);
}

static int dir_remove_name(uint32_t dir_ino, const char *name) {
    struct srfs_inode dip;
    if (!inode_get(dir_ino, &dip)) return FS_ERR_IO;
    uint32_t count = dip.size / 64u;
    for (uint32_t i = 0; i < count; i++) {
        struct srfs_dirent de;
        int r = dir_get_entry(dir_ino, i, &de);
        if (r < 0) return r;
        if (de.ino && kstrcmp(de.name, name) == 0) {
            de.ino = 0;                 /* 只清 ino，槽位可复用 */
            de.name[0] = 0;
            return dir_set_entry(dir_ino, i, &de);
        }
    }
    return FS_ERR_NOENT;
}

/* 原地改写目录项名字（同目录重命名，不改变 inode 和槽位）。 */
static int dir_rename_name(uint32_t dir_ino, const char *oldname,
                           const char *newname) {
    struct srfs_inode dip;
    if (!inode_get(dir_ino, &dip)) return FS_ERR_IO;
    if (dip.mode != FS_TYPE_DIR) return FS_ERR_NOTDIR;
    uint32_t count = dip.size / 64u;
    for (uint32_t i = 0; i < count; i++) {
        struct srfs_dirent de;
        int r = dir_get_entry(dir_ino, i, &de);
        if (r < 0) return r;
        if (de.ino && kstrcmp(de.name, oldname) == 0) {
            memset(de.name, 0, sizeof(de.name));
            kstrcpy(de.name, newname);
            return dir_set_entry(dir_ino, i, &de);
        }
    }
    return FS_ERR_NOENT;
}

/* 目录是否只含 . 和 .. */
static int dir_is_empty(uint32_t dir_ino, bool *empty) {
    struct srfs_inode dip;
    if (!inode_get(dir_ino, &dip)) return FS_ERR_IO;
    uint32_t count = dip.size / 64u;
    for (uint32_t i = 0; i < count; i++) {
        struct srfs_dirent de;
        int r = dir_get_entry(dir_ino, i, &de);
        if (r < 0) return r;
        if (de.ino && kstrcmp(de.name, ".") && kstrcmp(de.name, "..")) {
            *empty = false;
            return FS_OK;
        }
    }
    *empty = true;
    return FS_OK;
}

/* ---------------- 路径解析 ----------------

   成功：*out_ino = 最终 inode；若 parent_out 非空，同时给出最后一级的
   父目录 ino 与名字（供 remove/rename 使用）。
   不存在：FS_ERR_NOENT；若缺失的是最后一级，同样给出父目录与名字，
   可直接用于创建。 */
static int path_resolve(const char *path, uint32_t *out_ino,
                        uint32_t *parent_out, char *last_out) {
    if (!ready) return FS_ERR_IO;
    if (!path || !path[0]) path = "/";
    if (parent_out) *parent_out = 0;

    uint32_t cur = sb.root_inode;       /* 公共入口已把相对路径规范化为绝对路径 */
    char comp[FS_NAME_MAX + 1];
    const char *p = path;

    if (*p == '/') p++;
    while (*p) {
        int n = 0;
        while (*p && *p != '/' && n < FS_NAME_MAX) comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '/') p++;
        if (n == 0) continue;

        struct srfs_inode ip;
        if (!inode_get(cur, &ip)) return FS_ERR_IO;
        if (ip.mode != FS_TYPE_DIR) return FS_ERR_NOTDIR;

        uint32_t child = 0;
        int r = dir_lookup(cur, comp, &child);
        if (r == FS_ERR_NOENT) {
            if (!*p) {                  /* 缺失的恰是最后一级 */
                if (parent_out) *parent_out = cur;
                if (last_out) kstrcpy(last_out, comp);
            }
            return FS_ERR_NOENT;
        }
        if (r < 0) return r;
        if (!*p) {                      /* 已存在的最后一级也要给出父目录和名字 */
            if (parent_out) *parent_out = cur;
            if (last_out) kstrcpy(last_out, comp);
        }
        cur = child;
    }
    *out_ino = cur;
    return FS_OK;
}

static void str_append(char *dst, const char *src) {
    size_t l = kstrlen(dst);
    size_t n = kstrlen(src);
    if (l + n + 1 >= FS_PATH_MAX) return;
    kstrcpy(dst + l, src);
}

/* 纯字符串规范化：折叠 "."、".."、重复斜杠（结果以 / 开头） */
static void path_canon(const char *path, char *out) {
    char work[FS_PATH_MAX];
    if (path[0] == '/') {
        kstrcpy(work, path);
    } else {
        kstrcpy(work, cwd);
        size_t l = kstrlen(work);
        if (l && work[l - 1] != '/') { work[l] = '/'; work[l + 1] = 0; }
        str_append(work, path);
    }
    out[0] = '/';
    out[1] = 0;
    const char *p = work;
    while (*p) {
        char comp[FS_NAME_MAX + 1];
        int n = 0;
        while (*p && *p != '/' && n < FS_NAME_MAX) comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '/') p++;
        if (n == 0 || !kstrcmp(comp, ".")) continue;
        if (!kstrcmp(comp, "..")) {     /* 弹出一级 */
            char *slash = 0;
            for (char *q = out; *q; q++) if (*q == '/') slash = q;
            if (slash && slash != out) *slash = 0;
            continue;
        }
        if (out[1] == 0) str_append(out, comp);      /* 根下直接拼 */
        else { str_append(out, "/"); str_append(out, comp); }
    }
}

/* 把外部相对路径（相对全局 cwd）规范化为绝对路径。返回值可能指向 buf。 */
static const char *abs_path(const char *path, char *buf) {
    if (!path || !path[0]) return "/";
    if (path[0] == '/') return path;
    path_canon(path, buf);
    return buf;
}

/* 找到绝对路径所属的 SRFS 卷，并给出驱动视角的子路径。 */
static struct srfs_vol *vol_for_abs(const char *abs, const char **sub) {
    if (!abs) return 0;
    void *ctx = 0;
    size_t ml = 1;
    const struct filesystem_ops *ops = vfs_route(abs, &ctx, &ml);
    if (!ops || ops != &srfs_ops || !ctx) return 0;
    if (sub) *sub = vfs_subpath(abs, ml);
    return (struct srfs_vol *)ctx;
}

/* ---------------- 公共 API ---------------- */

static int srfs_listdir(void *ctx, const char *path, struct fs_dirent *out, int max) {
    g_cur = (struct srfs_vol *)ctx;
    if (!g_cur || !ready) return FS_ERR_IO;
    uint32_t ino = 0;
    int r = path_resolve(path, &ino, 0, 0);
    if (r < 0) return r;
    struct srfs_inode ip;
    if (!inode_get(ino, &ip)) return FS_ERR_IO;
    if (ip.mode != FS_TYPE_DIR) return FS_ERR_NOTDIR;

    int n = 0;
    uint32_t count = ip.size / 64u;
    for (uint32_t i = 0; i < count && n < max; i++) {
        struct srfs_dirent de;
        r = dir_get_entry(ino, i, &de);
        if (r < 0) return r;
        if (!de.ino) continue;
        if (!kstrcmp(de.name, ".") || !kstrcmp(de.name, "..")) continue;
        struct srfs_inode ci;
        if (!inode_get(de.ino, &ci)) return FS_ERR_IO;
        out[n].ino = de.ino;
        out[n].type = (uint8_t)ci.mode;
        out[n].size = ci.size;
        out[n].mtime = ci.mtime;
        kstrcpy(out[n].name, de.name);
        n++;
    }
    return n;
}

static int srfs_stat(void *ctx, const char *path, struct fs_dirent *out) {
    g_cur = (struct srfs_vol *)ctx;
    if (!g_cur || !ready) return FS_ERR_IO;
    uint32_t ino = 0;
    char last[FS_NAME_MAX + 1] = "/";
    int r = path_resolve(path, &ino, 0, last);
    if (r < 0) return r;
    struct srfs_inode ip;
    if (!inode_get(ino, &ip)) return FS_ERR_IO;
    out->ino = ino;
    out->type = (uint8_t)ip.mode;
    out->size = ip.size;
    out->mtime = ip.mtime;

    /* 取路径最后一段作为名字（path_resolve 只在缺失时填 last） */
    const char *base = path ? path : "/";
    if (base[0]) {
        const char *s = base, *b = base;
        for (; *s; s++) if (*s == '/' && s[1]) b = s + 1;
        if (*b && !(b[0] == '/' && b[1] == 0)) {
            size_t n = 0;
            while (b[n] && b[n] != '/' && n < FS_NAME_MAX) {
                out->name[n] = b[n];
                n++;
            }
            out->name[n] = 0;
            if (n) return FS_OK;
        }
    }
    kstrcpy(out->name, "/");
    return FS_OK;
}

bool fs_exists(const char *path) {
    char ab[FS_PATH_MAX];
    const char *abs = abs_path(path, ab);
    const char *sub = 0;
    struct srfs_vol *v = vol_for_abs(abs, &sub);
    if (!v) return false;
    g_cur = v;
    uint32_t ino = 0;
    return path_resolve(sub, &ino, 0, 0) == FS_OK;
}

static int srfs_read(void *ctx, const char *path, void *buf,
                     uint64_t count, uint64_t off) {
    g_cur = (struct srfs_vol *)ctx;
    if (!g_cur || !ready) return FS_ERR_IO;
    uint32_t ino = 0;
    int r = path_resolve(path, &ino, 0, 0);
    if (r < 0) return r;
    struct srfs_inode ip;
    if (!inode_get(ino, &ip)) return FS_ERR_IO;
    if (ip.mode == FS_TYPE_DIR) return FS_ERR_ISDIR;
    if (off > 0xFFFFFFFFu || count > 0xFFFFFFFFu) return FS_ERR_INVAL;
    return inode_read_at(&ip, (uint8_t *)buf, (uint32_t)count, (uint32_t)off);
}

static int srfs_write(void *ctx, const char *path, const void *buf,
                      uint64_t count, uint64_t off) {
    g_cur = (struct srfs_vol *)ctx;
    if (!g_cur || !ready) return FS_ERR_IO;
    uint32_t ino = 0;
    int r = path_resolve(path, &ino, 0, 0);
    if (r < 0) return r;
    struct srfs_inode ip;
    if (!inode_get(ino, &ip)) return FS_ERR_IO;
    if (ip.mode == FS_TYPE_DIR) return FS_ERR_ISDIR;
    if (off > 0xFFFFFFFFu || count > 0xFFFFFFFFu) return FS_ERR_INVAL;
    r = inode_write_at(ino, &ip, (uint32_t)off, (const uint8_t *)buf,
                       (uint32_t)count);
    if (r < 0) return r;
    fs_sync();
    return (int)count;
}

int fs_create(const char *path, uint8_t type) {
    if (type != FS_TYPE_FILE && type != FS_TYPE_DIR) return FS_ERR_INVAL;
    char ab[FS_PATH_MAX];
    const char *abs = abs_path(path, ab);
    const char *sub = 0;
    struct srfs_vol *v = vol_for_abs(abs, &sub);
    if (!v) return FS_ERR_NOENT;
    g_cur = v;
    if (!ready) return FS_ERR_IO;

    uint32_t ino = 0, parent = 0;
    char last[FS_NAME_MAX + 1];
    int r = path_resolve(sub, &ino, &parent, last);
    if (r == FS_OK) return FS_ERR_EXIST;
    if (r != FS_ERR_NOENT) return r;
    if (!parent || !last[0]) return FS_ERR_NOENT;
    size_t nl = kstrlen(last);
    if (nl == 0 || nl >= FS_NAME_MAX) return FS_ERR_INVAL;

    uint32_t child = 0;
    r = inode_alloc(&child);
    if (r < 0) return r;

    struct srfs_inode ip;
    memset(&ip, 0, sizeof(ip));
    ip.mode = type;
    ip.nlink = 1;
    ip.mtime = fs_now_stamp();
    if (!inode_put(child, &ip)) return FS_ERR_IO;

    if (type == FS_TYPE_DIR) {
        r = dir_add(child, ".", child);
        if (r >= 0) r = dir_add(child, "..", parent);
        if (r < 0) {
            inode_truncate(child, &ip);
            bitmap_set(INODE_BMAP_BLOCK, child, 0);
            return r;
        }
    }
    r = dir_add(parent, last, child);
    if (r < 0) {
        inode_truncate(child, &ip);
        bitmap_set(INODE_BMAP_BLOCK, child, 0);
        return r;
    }
    fs_sync();
    return FS_OK;
}

int fs_remove(const char *path) {
    char ab[FS_PATH_MAX];
    const char *abs = abs_path(path, ab);
    const char *sub = 0;
    struct srfs_vol *v = vol_for_abs(abs, &sub);
    if (!v) return FS_ERR_NOENT;
    g_cur = v;
    if (!ready) return FS_ERR_IO;

    uint32_t ino = 0, parent = 0;
    char last[FS_NAME_MAX + 1];
    int r = path_resolve(sub, &ino, &parent, last);
    if (r < 0) return r;
    if (!parent) return FS_ERR_INVAL;            /* 根目录不可删 */
    if (!kstrcmp(last, ".") || !kstrcmp(last, "..")) return FS_ERR_INVAL;
    if (!kstrcmp(abs, cwd)) return FS_ERR_INVAL; /* 当前目录不可删 */

    struct srfs_inode ip;
    if (!inode_get(ino, &ip)) return FS_ERR_IO;

    if (ip.mode == FS_TYPE_DIR) {
        bool empty = false;
        r = dir_is_empty(ino, &empty);
        if (r < 0) return r;
        if (!empty) return FS_ERR_NOTEMPTY;
    }
    r = dir_remove_name(parent, last);
    if (r < 0) return r;
    r = inode_truncate(ino, &ip);
    if (r < 0) return r;
    if (ip.nlink) ip.nlink--;
    r = bitmap_set(INODE_BMAP_BLOCK, ino, 0);
    if (r < 0) return r;

    if (ip.mode == FS_TYPE_DIR) {               /* 父目录 nlink-- */
        struct srfs_inode pp;
        if (inode_get(parent, &pp) && pp.nlink) {
            pp.nlink--;
            inode_put(parent, &pp);
        }
    }
    fs_sync();
    return FS_OK;
}

int fs_rename(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath || !oldpath[0] || !newpath[0])
        return FS_ERR_INVAL;

    char abo[FS_PATH_MAX], abn[FS_PATH_MAX];
    const char *oldabs = abs_path(oldpath, abo);
    const char *newabs = abs_path(newpath, abn);
    const char *oldsub = 0, *newsub = 0;
    struct srfs_vol *v = vol_for_abs(oldabs, &oldsub);
    struct srfs_vol *nv = vol_for_abs(newabs, &newsub);
    if (!v || !nv || v != nv) return FS_ERR_INVAL;
    g_cur = v;
    if (!ready) return FS_ERR_IO;

    uint32_t ino = 0, parent = 0;
    char oldlast[FS_NAME_MAX + 1] = { 0 };
    int r = path_resolve(oldsub, &ino, &parent, oldlast);
    if (r < 0) return r;
    if (!parent || !oldlast[0]) return FS_ERR_INVAL;
    if (!kstrcmp(oldlast, ".") || !kstrcmp(oldlast, ".."))
        return FS_ERR_INVAL;
    if (!kstrcmp(oldabs, cwd)) return FS_ERR_INVAL;  /* 当前目录不可改名 */

    uint32_t exist = 0, newparent = 0;
    char newlast[FS_NAME_MAX + 1] = { 0 };
    r = path_resolve(newsub, &exist, &newparent, newlast);
    if (r == FS_OK) {
        return (exist == ino) ? FS_OK : FS_ERR_EXIST;
    }
    if (r != FS_ERR_NOENT) return r;
    if (!newparent || newparent != parent || !newlast[0])
        return FS_ERR_INVAL;                     /* 仅支持同目录重命名 */
    if (kstrlen(newlast) == 0 || kstrlen(newlast) >= FS_NAME_MAX)
        return FS_ERR_INVAL;
    if (!kstrcmp(oldlast, newlast)) return FS_OK;

    r = dir_rename_name(parent, oldlast, newlast);
    if (r < 0) return r;

    struct srfs_inode ip;
    if (inode_get(ino, &ip)) {
        ip.mtime = fs_now_stamp();
        inode_put(ino, &ip);
    }
    fs_sync();
    return FS_OK;
}

int fs_write_file(const char *path, const void *buf, uint64_t count) {
    char ab[FS_PATH_MAX];
    const char *abs = abs_path(path, ab);
    const char *sub = 0;
    struct srfs_vol *v = vol_for_abs(abs, &sub);
    if (!v) return FS_ERR_NOENT;
    g_cur = v;
    if (!ready) return FS_ERR_IO;

    uint32_t ino = 0;
    int r = path_resolve(sub, &ino, 0, 0);
    if (r == FS_ERR_NOENT) {
        r = fs_create(abs, FS_TYPE_FILE);
        if (r < 0) return r;
        g_cur = v;                   /* fs_create 可能切换了 g_cur */
        r = path_resolve(sub, &ino, 0, 0);
        if (r < 0) return r;
    } else if (r < 0) return r;

    struct srfs_inode ip;
    if (!inode_get(ino, &ip)) return FS_ERR_IO;
    if (ip.mode == FS_TYPE_DIR) return FS_ERR_ISDIR;

    r = inode_truncate(ino, &ip);               /* 截断后整体写入 */
    if (r < 0) return r;
    if (!inode_get(ino, &ip)) return FS_ERR_IO;
    if (count > 0) {
        r = inode_write_at(ino, &ip, 0, (const uint8_t *)buf, (uint32_t)count);
        if (r < 0) return r;
    }
    fs_sync();
    return (int)count;
}

int fs_chdir(const char *path) {
    char ab[FS_PATH_MAX];
    const char *abs = abs_path(path, ab);
    const char *sub = 0;
    struct srfs_vol *v = vol_for_abs(abs, &sub);
    if (!v) return FS_ERR_NOENT;
    g_cur = v;
    if (!ready) return FS_ERR_IO;

    uint32_t ino = 0;
    int r = path_resolve(sub, &ino, 0, 0);
    if (r < 0) return r;
    struct srfs_inode ip;
    if (!inode_get(ino, &ip)) return FS_ERR_IO;
    if (ip.mode != FS_TYPE_DIR) return FS_ERR_NOTDIR;
    char newcwd[FS_PATH_MAX];
    path_canon(abs, newcwd);
    kstrcpy(cwd, newcwd);
    return FS_OK;
}

const char *fs_getcwd(void) { return cwd; }

int fs_statfs_path(const char *path, uint64_t *total_blocks,
                   uint64_t *free_blocks, uint64_t *total_inodes,
                   uint64_t *free_inodes) {
    char ab[FS_PATH_MAX];
    const char *abs = abs_path(path, ab);
    const char *sub = 0;
    struct srfs_vol *v = vol_for_abs(abs, &sub);
    (void)sub;
    if (!v) return FS_ERR_NOENT;
    g_cur = v;
    if (!ready) return FS_ERR_IO;
    if (total_blocks) *total_blocks = sb.total_blocks;
    if (free_blocks)  *free_blocks  = sb.free_blocks;
    if (total_inodes) *total_inodes = sb.total_inodes;
    if (free_inodes)  *free_inodes  = sb.free_inodes;
    return FS_OK;
}

int fs_statfs(uint64_t *total_blocks, uint64_t *free_blocks,
              uint64_t *total_inodes, uint64_t *free_inodes) {
    if (!fs_ready()) return FS_ERR_IO;
    return fs_statfs_path("/", total_blocks, free_blocks,
                          total_inodes, free_inodes);
}

const char *fs_strerror(int err) {
    switch (err) {
    case FS_OK:            return "成功";
    case FS_ERR_NOENT:     return "文件或目录不存在";
    case FS_ERR_EXIST:     return "文件已存在";
    case FS_ERR_NOTDIR:    return "不是目录";
    case FS_ERR_ISDIR:     return "是目录";
    case FS_ERR_NOTEMPTY:  return "目录非空";
    case FS_ERR_INVAL:     return "参数无效";
    case FS_ERR_NOSPACE:   return "空间不足";
    case FS_ERR_IO:        return "I/O 错误";
    default:               return "未知错误";
    }
}

static void buf_mark_dirty(struct buf *b) { b->state = 2; }

void fs_sync(void) {
    if (!g_cur || !ready) return;
    int failed = 0;
    for (int i = 0; i < NCACHE; i++) {
        if (cache[i].state == 2) {
            if (disk_write_block(cache[i].blk, cache[i].data))
                cache[i].state = 1;
            else
                failed++;         /* 写失败的脏块保持 dirty，下次 sync 再试 */
        }
    }
    if (failed) {
        serial_puts("[SRCore] sync: ");
        serial_putdec((uint64_t)failed);
        serial_puts(" dirty block(s) FAILED to write\n");
        return;                   /* 有块没写成功，此时 FLUSH 也无意义 */
    }
    if (raw_dirty) {
        if (blkdev_flush(g_cur->bdev_idx))
            raw_dirty = false;    /* 只有真刷成功才清账，失败留给下次 sync 重试 */
        else
            serial_puts("[SRCore] FLUSH CACHE EXT failed (data may stay in drive cache)\n");
    }
}

static void super_mark_dirty(void) {
    struct buf *b = buf_get(SUPER_BLOCK);
    if (b) {
        memcpy(b->data, &sb, sizeof(sb));
        buf_mark_dirty(b);
    }
}

/* ============================================================
 *  阶段二：VFS 包装
 *
 *  SRFS 的四个「通用」操作（listdir/stat/read/write）登记成
 *  filesystem_ops，默认挂载在 "/"。公开的 fs_xxx 保持原签名，
 *  内部改为经 VFS 最长前缀路由转发——以后把 FAT32 挂到 /mnt/usb，
 *  同一套 fs_read()/fs_listdir() 就会自动落到 FAT32 驱动。
 *
 *  SRFS 专有语义（create/remove/chdir/statfs/format/sync）不做通用化：
 *  它们在 fs.h 中保留原实现，直接操作 SRFS 的全局状态。
 * ============================================================ */

static const struct filesystem_ops srfs_ops = {
    .name    = "srfs",
    .mount   = srfs_mount_op,
    .umount  = srfs_umount_op,
    .listdir = srfs_listdir,
    .stat    = srfs_stat,
    .read    = srfs_read,
    .write   = srfs_write,
};

/* ---- 向下兼容的公开 API：内部全部经 VFS 路由 ---- */

int fs_listdir(const char *path, struct fs_dirent *out, int max) {
    char ab[FS_PATH_MAX];
    return vfs_listdir(abs_path(path, ab), out, max);
}

int fs_stat(const char *path, struct fs_dirent *out) {
    char ab[FS_PATH_MAX];
    return vfs_stat(abs_path(path, ab), out);
}

int fs_read(const char *path, void *buf, uint64_t count, uint64_t off) {
    char ab[FS_PATH_MAX];
    return vfs_read(abs_path(path, ab), buf, count, off);
}

int fs_write(const char *path, const void *buf, uint64_t count, uint64_t off) {
    char ab[FS_PATH_MAX];
    int r = vfs_write(abs_path(path, ab), buf, count, off);
    /* fs_write 的公开语义是“返回写入字节数”，VFS 也是同样约定。 */
    return r;
}
