#include "blkdev.h"
#include "drivers/ahci.h"
#include "fs/partition.h"
#include "drivers/serial.h"
#include "lib/mem.h"
#include "lib/string.h"

static struct block_device g_blkdevs[BLKDEV_MAX];
static int                 g_blkdev_count = 0;

static void make_name(char *out, size_t cap, int disk_idx, int part) {
    /* 不依赖 snprintf：内核只有一个很小的字符串库。 */
    char tmp[24];
    size_t n = 0;
    const char *a = "disk";
    while (*a && n + 1 < cap) out[n++] = *a++;
    char *p = utoa((uint64_t)disk_idx, tmp, 10);
    while (*p && n + 1 < cap) out[n++] = *p++;
    if (n + 1 < cap) out[n++] = 'p';
    p = utoa((uint64_t)part, tmp, 10);
    while (*p && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
}

void blkdev_refresh(void) {
    g_blkdev_count = 0;
    memset(g_blkdevs, 0, sizeof(g_blkdevs));

    for (int i = 0; i < partition_count() && g_blkdev_count < BLKDEV_MAX; i++) {
        const struct partition_info *pi = partition_get(i);
        if (!pi || !pi->present) continue;

        struct block_device *b = &g_blkdevs[g_blkdev_count];
        make_name(b->name, sizeof(b->name), pi->disk_idx, pi->index);
        b->ahci_disk_idx = pi->disk_idx;
        b->start_lba     = pi->start_lba;
        b->sector_count  = pi->sectors;
        b->part_index    = pi->index;
        b->part_type     = pi->type;
        b->present       = true;
        g_blkdev_count++;

        serial_puts("[SRCore] blkdev: ");
        serial_puts(b->name);
        serial_puts(" start=");
        serial_putdec(b->start_lba);
        serial_puts(" sectors=");
        serial_putdec(b->sector_count);
        serial_puts("\n");
    }
}

int blkdev_scan(void) {
    partition_scan();
    blkdev_refresh();
    return g_blkdev_count;
}

int blkdev_count(void) { return g_blkdev_count; }

const struct block_device *blkdev_get(int idx) {
    if (idx < 0 || idx >= g_blkdev_count) return 0;
    return &g_blkdevs[idx];
}

const struct block_device *blkdev_find(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < g_blkdev_count; i++)
        if (!kstrcmp(g_blkdevs[i].name, name)) return &g_blkdevs[i];
    return 0;
}

const struct block_device *blkdev_find_part(int disk_idx, uint32_t start_lba) {
    for (int i = 0; i < g_blkdev_count; i++)
        if (g_blkdevs[i].ahci_disk_idx == disk_idx &&
            g_blkdevs[i].start_lba == start_lba)
            return &g_blkdevs[i];
    return 0;
}

int blkdev_index_of(const struct block_device *dev) {
    if (!dev) return -1;
    for (int i = 0; i < g_blkdev_count; i++)
        if (&g_blkdevs[i] == dev) return i;
    return -1;
}

bool blkdev_read(int idx, uint64_t lba, uint32_t count, void *buf) {
    if (idx < 0 || idx >= g_blkdev_count || !buf || count == 0) return false;
    const struct block_device *b = &g_blkdevs[idx];
    if (!b->present) return false;
    if (lba >= b->sector_count ||
        (uint64_t)count > b->sector_count - lba)
        return false;
    return ahci_read_sectors(b->ahci_disk_idx, b->start_lba + lba, count, buf);
}

bool blkdev_write(int idx, uint64_t lba, uint32_t count, const void *buf) {
    if (idx < 0 || idx >= g_blkdev_count || !buf || count == 0) return false;
    const struct block_device *b = &g_blkdevs[idx];
    if (!b->present) return false;
    if (lba >= b->sector_count ||
        (uint64_t)count > b->sector_count - lba)
        return false;
    return ahci_write_sectors(b->ahci_disk_idx, b->start_lba + lba, count, buf);
}

bool blkdev_flush(int idx) {
    if (idx < 0 || idx >= g_blkdev_count) return false;
    return ahci_flush_disk(g_blkdevs[idx].ahci_disk_idx);
}

bool blkdev_read_dev(const struct block_device *dev, uint64_t lba,
                     uint32_t count, void *buf) {
    return blkdev_read(blkdev_index_of(dev), lba, count, buf);
}

bool blkdev_write_dev(const struct block_device *dev, uint64_t lba,
                      uint32_t count, const void *buf) {
    return blkdev_write(blkdev_index_of(dev), lba, count, buf);
}
