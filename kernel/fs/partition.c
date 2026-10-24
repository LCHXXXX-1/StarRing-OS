#include "partition.h"
#include "drivers/ahci.h"
#include "drivers/pci.h"
#include "drivers/serial.h"
#include "lib/mem.h"
#include "lib/string.h"

/* 所有物理盘的分区解析结果 */
struct partition_info g_partitions[PART_MAX];
int                   g_partition_count = 0;
struct partition_disk g_partition_disks[PART_MAX_DISKS];
int                   g_partition_disk_count = 0;

static bool g_mbr_valid = false;

/* 单扇区读缓冲：静态、4096 对齐（AHCI 驱动内部还会走 bounce，这里只是
   保证任何实现下都不会踩到未对齐 DMA 的限制）。禁止 malloc。 */
static uint8_t mbr_buf[512] __attribute__((aligned(4096)));

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* 确保 AHCI 已经初始化。启动早期分区扫描必须先拉起控制器。 */
static bool ensure_disks(void) {
    if (ahci_present()) return true;

    uint32_t abar = pci_scan();
    if (!abar) {
        serial_puts("[SRCore] partition: AHCI controller not found\n");
        return false;
    }
    if (!ahci_init(abar)) {
        serial_puts("[SRCore] partition: AHCI init failed\n");
        return false;
    }
    return true;
}

bool partition_scan(void) {
    g_partition_count      = 0;
    g_partition_disk_count = 0;
    g_mbr_valid            = false;
    memset(g_partitions, 0, sizeof(g_partitions));
    memset(g_partition_disks, 0, sizeof(g_partition_disks));

    if (!ensure_disks()) return false;

    int ndisks = ahci_disk_count();
    if (ndisks > PART_MAX_DISKS) ndisks = PART_MAX_DISKS;
    g_partition_disk_count = ndisks;

    for (int d = 0; d < ndisks; d++) {
        struct partition_disk *di = &g_partition_disks[d];
        di->ahci_idx = d;
        di->sectors  = ahci_disk_sectors(d);
        di->present  = di->sectors > 0;
        di->mbr_valid = false;
        di->part_count = 0;

        if (!di->present) continue;

        if (!ahci_read_sectors(d, 0, 1, mbr_buf)) {
            serial_puts("[SRCore] partition: disk");
            serial_putdec((uint64_t)d);
            serial_puts(" failed to read MBR (LBA 0)\n");
            continue;
        }

        if (mbr_buf[510] != 0x55 || mbr_buf[511] != 0xAA) {
            serial_puts("[SRCore] partition: disk");
            serial_putdec((uint64_t)d);
            serial_puts(" has no 0x55AA signature\n");
            continue;
        }

        di->mbr_valid = true;
        g_mbr_valid   = true;

        for (int i = 0; i < PART_MAX_PER_DISK; i++) {
            const uint8_t *e = mbr_buf + 0x1BE + i * 16;
            uint8_t  type    = e[4];
            uint32_t start   = rd32(e + 8);
            uint32_t sectors = rd32(e + 12);

            if (type == PART_TYPE_EMPTY || sectors == 0) continue;

            /* 防御性检查：分区必须落在磁盘范围内，且不能从 LBA 0（MBR 本身）
               开始，否则一次误格式化就会把分区表也抹掉。 */
            if (start == 0) {
                serial_puts("[SRCore] partition: disk");
                serial_putdec((uint64_t)d);
                serial_puts(" slot ");
                serial_putdec((uint64_t)(i + 1));
                serial_puts(" starts at LBA 0, ignored\n");
                continue;
            }
            if ((uint64_t)start + (uint64_t)sectors > di->sectors) {
                serial_puts("[SRCore] partition: disk");
                serial_putdec((uint64_t)d);
                serial_puts(" slot ");
                serial_putdec((uint64_t)(i + 1));
                serial_puts(" exceeds disk bounds, ignored\n");
                continue;
            }

            struct partition_info *p = &g_partitions[g_partition_count++];
            p->index        = (uint8_t)(i + 1);
            p->slot         = (uint8_t)i;
            p->type         = type;
            p->start_lba    = start;
            p->sectors      = sectors;
            p->disk_idx     = (uint8_t)d;
            p->disk_sectors = di->sectors;
            p->present      = true;
            di->part_count++;

            serial_puts("[SRCore] disk");
            serial_putdec((uint64_t)d);
            serial_puts(" partition ");
            serial_putdec((uint64_t)p->index);
            serial_puts(": type=");
            serial_puthex(type);
            serial_puts(" start=");
            serial_putdec(start);
            serial_puts(" sectors=");
            serial_putdec(sectors);
            serial_puts("\n");
        }
    }

    if (g_mbr_valid) {
        serial_puts("[SRCore] MBR scan ok, ");
        serial_putdec((uint64_t)g_partition_disk_count);
        serial_puts(" disk(s), ");
        serial_putdec((uint64_t)g_partition_count);
        serial_puts(" partition(s)\n");
    }
    return g_mbr_valid;
}

bool partition_valid(void) { return g_mbr_valid; }

int partition_count(void) { return g_partition_count; }

int partition_disk_count(void) { return g_partition_disk_count; }

const struct partition_disk *partition_disk_info(int i) {
    if (i < 0 || i >= g_partition_disk_count) return 0;
    return &g_partition_disks[i];
}

bool partition_create_mbr(int disk_idx, uint32_t start_lba, uint8_t type) {
    if (!ensure_disks()) return false;
    if (disk_idx < 0 || disk_idx >= ahci_disk_count()) return false;

    uint64_t disk_sectors = ahci_disk_sectors(disk_idx);
    /* 至少留 2 MiB 给 SRFS 元数据和根目录。 */
    if (start_lba < 1 || (uint64_t)start_lba + 4096u > disk_sectors) {
        serial_puts("[SRCore] partition: disk too small for a new MBR partition\n");
        return false;
    }

    uint64_t avail = disk_sectors - start_lba;
    if (avail > 0xFFFFFFFFull) avail = 0xFFFFFFFFull;
    uint32_t sectors = (uint32_t)avail;
    if (sectors == 0) return false;

    memset(mbr_buf, 0, sizeof(mbr_buf));
    wr32(mbr_buf + 0x1B8, 0x53523031u | ((uint32_t)disk_idx & 0xFFu));

    uint8_t *e = mbr_buf + 0x1BE;
    e[0] = 0x00;                              /* not bootable */
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;    /* CHS start (LBA mode) */
    e[4] = type;
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;    /* CHS end (LBA mode) */
    wr32(e + 8,  start_lba);
    wr32(e + 12, sectors);
    mbr_buf[510] = 0x55;
    mbr_buf[511] = 0xAA;

    if (!ahci_write_sectors(disk_idx, 0, 1, mbr_buf)) {
        serial_puts("[SRCore] partition: failed to write MBR at LBA 0\n");
        return false;
    }
    if (!ahci_flush_disk(disk_idx)) {
        serial_puts("[SRCore] partition: FLUSH after MBR write failed\n");
        return false;
    }

    serial_puts("[SRCore] partition: wrote MBR on disk");
    serial_putdec((uint64_t)disk_idx);
    serial_puts(" type=");
    serial_puthex(type);
    serial_puts(" start=");
    serial_putdec(start_lba);
    serial_puts(" sectors=");
    serial_putdec(sectors);
    serial_puts("\n");

    if (!partition_scan()) return false;
    return partition_by_disk_start(disk_idx, start_lba) != 0;
}

bool partition_disk_is_blank(int disk_idx) {
    if (!ensure_disks()) return false;
    if (disk_idx < 0 || disk_idx >= ahci_disk_count()) return false;

    uint64_t disk_sectors = ahci_disk_sectors(disk_idx);
    if (disk_sectors < (uint64_t)PART_DEFAULT_START + 4096u) return false;

    /* 前 1 MiB 必须全零。这样既排除已有引导扇区/分区表，
       也排除旧版“SRFS 直接从 LBA 0 开始”的镜像（超级块在 LBA 8）。 */
    static uint8_t scan_buf[4096] __attribute__((aligned(4096)));
    for (uint32_t lba = 0; lba < PART_DEFAULT_START; lba += 8) {
        if (!ahci_read_sectors(disk_idx, lba, 8, scan_buf)) {
            serial_puts("[SRCore] partition: blank check read failed\n");
            return false;
        }
        for (uint32_t i = 0; i < sizeof(scan_buf); i++) {
            if (scan_buf[i]) {
                serial_puts("[SRCore] partition: disk");
                serial_putdec((uint64_t)disk_idx);
                serial_puts(" not blank at LBA ");
                serial_putdec(lba);
                serial_puts("\n");
                return false;
            }
        }
    }
    return true;
}

bool partition_disk0_is_blank(void) { return partition_disk_is_blank(0); }
bool partition_create_mbr_disk0(uint32_t start_lba, uint8_t type) {
    return partition_create_mbr(0, start_lba, type);
}

const struct partition_info *partition_get(int i) {
    if (i < 0 || i >= g_partition_count) return 0;
    return &g_partitions[i];
}

const struct partition_info *partition_find_type(uint8_t type) {
    for (int i = 0; i < g_partition_count; i++)
        if (g_partitions[i].type == type) return &g_partitions[i];
    return 0;
}

const struct partition_info *partition_find_srfs(void) {
    const struct partition_info *p = partition_find_type(PART_TYPE_SRFS);
    if (!p) p = partition_find_type(PART_TYPE_LINUX);
    return p;
}

const struct partition_info *partition_by_start(uint32_t start_lba) {
    for (int i = 0; i < g_partition_count; i++)
        if (g_partitions[i].start_lba == start_lba) return &g_partitions[i];
    return 0;
}

const struct partition_info *partition_by_disk_start(int disk_idx,
                                                     uint32_t start_lba) {
    for (int i = 0; i < g_partition_count; i++)
        if (g_partitions[i].disk_idx == disk_idx &&
            g_partitions[i].start_lba == start_lba)
            return &g_partitions[i];
    return 0;
}

const char *partition_type_name(uint8_t type) {
    switch (type) {
    case PART_TYPE_SRFS:   return "SRFS";
    case PART_TYPE_LINUX:  return "Linux";
    case 0x0B:             return "FAT32";
    case 0x0C:             return "FAT32";
    case 0x07:             return "NTFS/exFAT";
    case 0x05:             return "Extended";
    case 0x0F:             return "Extended";
    case 0x82:             return "Linux swap";
    case 0xEF:             return "EFI System";
    default:               return "Unknown";
    }
}
