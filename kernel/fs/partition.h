#ifndef PARTITION_H
#define PARTITION_H
#include <stdint.h>
#include <stdbool.h>

/* ============================================================
 *  MBR 分区表解析（每块物理盘最多 4 个主分区）
 *
 *  LBA 0 的 512 字节扇区布局：
 *      0x1BE + i*16 : 第 i 个分区表项（i = 0..3）
 *          偏移 0  : 引导标志
 *          偏移 4  : 分区类型
 *          偏移 8  : 起始 LBA（小端 32 位）
 *          偏移 12 : 扇区总数（小端 32 位）
 *      0x1FE        : 0x55 0xAA 引导签名
 *
 *  多盘改造后，g_partitions 是所有物理盘分区表的扁平视图；
 *  每项通过 disk_idx / disk_sectors 记录来源盘，避免 start_lba
 *  在不同盘上重复时产生歧义。
 * ============================================================ */

#define PART_MAX_DISKS     32
#define PART_MAX_PER_DISK  4
#define PART_MAX           (PART_MAX_DISKS * PART_MAX_PER_DISK)

#define PART_TYPE_EMPTY 0x00
#define PART_TYPE_SRFS  0x53    /* StarRing OS 自定义分区类型 */
#define PART_TYPE_LINUX 0x83

/* 新分区默认从 1 MiB 对齐处开始，避开 LBA 0 的 MBR。 */
#define PART_DEFAULT_START 2048u

struct partition_info {
    uint8_t  index;             /* MBR 槽位号 1..4 */
    uint8_t  slot;              /* 槽位下标 0..3 */
    uint8_t  type;              /* MBR 分区类型 */
    uint8_t  disk_idx;          /* AHCI disk_idx */
    uint32_t start_lba;         /* 分区起始 LBA（相对物理盘） */
    uint32_t sectors;           /* 分区扇区总数 */
    uint64_t disk_sectors;      /* 来源物理盘总扇区数 */
    bool     present;           /* 有效表项 */
};

struct partition_disk {
    int      ahci_idx;
    uint64_t sectors;
    bool     present;
    bool     mbr_valid;
    int      part_count;        /* 本盘有效主分区数 */
};

/* 扫描结果（全局扁平数组） */
extern struct partition_info g_partitions[PART_MAX];
extern int                    g_partition_count;
extern struct partition_disk  g_partition_disks[PART_MAX_DISKS];
extern int                    g_partition_disk_count;

/* 扫描所有 AHCI 物理盘。返回至少一块盘有 MBR 时 true；
   无盘/全部读失败返回 false。 */
bool partition_scan(void);

/* 在指定物理盘上创建 MBR + 一个主分区（会重新扫描全部分区表）。 */
bool partition_create_mbr(int disk_idx, uint32_t start_lba, uint8_t type);

/* 前 1 MiB 是否全零（用于是否自动初始化空白盘）。 */
bool partition_disk_is_blank(int disk_idx);

/* 兼容 disk0 的单盘入口。 */
bool partition_disk0_is_blank(void);
bool partition_create_mbr_disk0(uint32_t start_lba, uint8_t type);

bool partition_valid(void);     /* 任意物理盘存在有效 MBR */

int  partition_count(void);     /* 所有盘上的有效分区总数 */
const struct partition_info *partition_get(int i);

int  partition_disk_count(void);
const struct partition_disk *partition_disk_info(int i);

const struct partition_info *partition_find_type(uint8_t type);
const struct partition_info *partition_find_srfs(void);   /* 0x53 优先，其次 0x83 */
const struct partition_info *partition_by_start(uint32_t start_lba);
const struct partition_info *partition_by_disk_start(int disk_idx,
                                                     uint32_t start_lba);

const char *partition_type_name(uint8_t type);

#endif
