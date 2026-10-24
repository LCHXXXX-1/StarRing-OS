#ifndef FS_H
#define FS_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* srfs：类 ext2 的简单磁盘文件系统
   布局（块大小 4096）：
     0 引导保留 | 1 superblock | 2 块位图 | 3 inode 位图
     4..N inode 表 | 其后数据区 */

#define FS_TYPE_FILE 1
#define FS_TYPE_DIR  2

#define FS_NAME_MAX  60
#define FS_PATH_MAX  128
#define FS_MAX_DIRENTS 64     /* 单次 fs_listdir 最多返回条目数 */

/* 错误码（负值） */
#define FS_OK          0
#define FS_ERR_NOENT  (-2)
#define FS_ERR_EXIST  (-3)
#define FS_ERR_NOTDIR (-4)
#define FS_ERR_ISDIR  (-5)
#define FS_ERR_NOTEMPTY (-6)
#define FS_ERR_INVAL  (-7)
#define FS_ERR_NOSPACE (-8)
#define FS_ERR_IO     (-9)

struct fs_dirent {
    uint32_t ino;
    uint8_t  type;
    uint64_t size;
    /* SRFS 修改时间：year-2000(6) | month(4) | day(5) | hour(5) | min(6) | sec(6)；
       0 = 未记录（旧磁盘或读取 RTC 失败）。 */
    uint32_t mtime;
    char     name[FS_NAME_MAX + 1];
};

/* 挂载所有 SRFS/Linux 分区：第一个可挂载者 → "/"，其余 → /mnt/<bdev>。
   返回成功挂载的卷数（0 表示根文件系统不可用）。 */
int fs_mount_all(void);

/* 兼容旧入口：按起始 LBA 确认分区存在后调用 fs_mount_all()。 */
bool fs_mount(uint64_t start_lba);

bool fs_ready(void);
uint64_t fs_active_start_lba(void);    /* 根卷分区起始 LBA（未挂载返回 0） */
int fs_root_bdev_index(void);          /* 根卷对应的 block_device 下标，-1 无 */
bool fs_bdev_is_mounted(int bdev_idx); /* 该块设备当前是否已作为 SRFS 卷挂载 */
void fs_bootstrap(void);               /* 补齐默认目录树（幂等） */
void fs_sync(void);                    /* 把缓存脏块回写磁盘 */

/* 破坏性：把当前根分区重新格式化成空的 SRFS。
   成功后文件系统立即失效（fs_ready() 变 false），必须重启才会重新挂载并
   由 fs_bootstrap() 生成默认目录树。成功返回 FS_OK，失败返回 FS_ERR_*。 */
int fs_format_disk(void);

typedef void (*fs_format_progress_fn)(int percent, void *user);

/* 带真实进度的指定块设备格式化。格式化活动根卷后 fs_ready() 变 false；
   格式化其它卷会自动重新挂载所有分区。 */
int fs_format_bdev(int bdev_idx, fs_format_progress_fn cb, void *user);

/* 带真实进度的分区格式化。
   start_lba 必须来自 MBR 分区表；任何块读写都会加上该偏移。格式化非当前
   挂载分区时，当前 SRFS 保持可用（缓存/超级块都不受影响）。
   progress 回调在 fs_format_disk_ex() 的调用上下文里同步触发——每写一个
   元数据块报一次，percent 单调递增（0..99 覆盖重建元数据，100 表示 FLUSH
   落盘完成）。磁盘工具把它跑在桌面主循环上，因此回调里可以安全地重绘界面。
   cb 允许为 NULL。 */
int fs_format_disk_ex(uint64_t start_lba, fs_format_progress_fn cb, void *user);

/* fs_listdir / fs_stat / fs_read / fs_write 四个通用操作由 VFS 做最长前缀
   路由（见 vfs.h）：/mnt/usb 等外部挂载点会自动命中对应驱动，其余路径落到
   挂在 / 的 SRFS。函数名与签名保持不变，现有调用方无需改动。
   fs_exists / fs_create / fs_remove / fs_chdir / fs_statfs 等 SRFS 专有语义
   仍直接落在 fs.c。 */
int  fs_listdir(const char *path, struct fs_dirent *out, int max);
int  fs_stat(const char *path, struct fs_dirent *out);
bool fs_exists(const char *path);
int  fs_read(const char *path, void *buf, uint64_t count, uint64_t off);
int  fs_write(const char *path, const void *buf, uint64_t count, uint64_t off);
/* 新建或截断后整体写入（echo 重定向用），返回写入字节数 */
int  fs_write_file(const char *path, const void *buf, uint64_t count);
int  fs_create(const char *path, uint8_t type);
int  fs_remove(const char *path);
/* 同一目录内重命名（oldpath/newpath 的父目录必须相同）。
   目标已存在时返回 FS_ERR_EXIST；其它失败返回 FS_ERR_*。 */
int  fs_rename(const char *oldpath, const char *newpath);

int  fs_chdir(const char *path);
const char *fs_getcwd(void);
int  fs_statfs(uint64_t *total_blocks, uint64_t *free_blocks,
               uint64_t *total_inodes, uint64_t *free_inodes);
int  fs_statfs_path(const char *path, uint64_t *total_blocks,
                    uint64_t *free_blocks, uint64_t *total_inodes,
                    uint64_t *free_inodes);
const char *fs_strerror(int err);
#endif