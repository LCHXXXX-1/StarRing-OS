#ifndef BLKDEV_H
#define BLKDEV_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ============================================================
 *  块设备抽象层
 *
 *  物理盘由 AHCI 管理；MBR 分区被登记为 block_device，文件系统
 *  只看到“分区内相对 LBA”，不再直接接触 AHCI 端口/物理盘号。
 *
 *  所有结构静态分配，禁止 malloc。
 * ============================================================ */

#define BLKDEV_MAX  (32 * 4)

struct block_device {
    char     name[32];          /* "disk0p1"（稳定、可打印） */
    int      ahci_disk_idx;     /* 来源 AHCI 物理盘 */
    uint64_t start_lba;         /* 分区起始 LBA（相对物理盘） */
    uint64_t sector_count;      /* 分区大小 */
    uint8_t  part_index;        /* MBR 槽位 1..4 */
    uint8_t  part_type;         /* MBR 类型 */
    bool     present;
};

/* 从当前 g_partitions 重建块设备表（不重新读 MBR）。 */
void blkdev_refresh(void);

/* 重新扫描所有物理盘的 MBR，再重建块设备表。返回块设备数量。 */
int  blkdev_scan(void);

int  blkdev_count(void);
const struct block_device *blkdev_get(int idx);
const struct block_device *blkdev_find(const char *name);
const struct block_device *blkdev_find_part(int disk_idx, uint32_t start_lba);
int  blkdev_index_of(const struct block_device *dev);

/* 读写以分区起始 LBA 为 0 的扇区。越界/无设备返回 false。 */
bool blkdev_read (int idx, uint64_t lba, uint32_t count, void *buf);
bool blkdev_write(int idx, uint64_t lba, uint32_t count, const void *buf);
bool blkdev_flush(int idx);

/* 便捷：按设备指针操作 */
bool blkdev_read_dev (const struct block_device *dev, uint64_t lba,
                      uint32_t count, void *buf);
bool blkdev_write_dev(const struct block_device *dev, uint64_t lba,
                      uint32_t count, const void *buf);

#endif
