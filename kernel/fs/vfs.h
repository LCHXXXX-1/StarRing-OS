#ifndef VFS_H
#define VFS_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "fs.h"        /* 复用 struct fs_dirent 与 FS_ERR_* 错误码 */

/* ============================================================
 *  VFS（Virtual File System）—— 挂载路由层
 *
 *  设计（阶段二）：
 *    - 每个具体文件系统（SRFS / FAT32 …）实现一张统一的
 *      struct filesystem_ops 操作表；
 *    - 全局挂载表把「挂载点路径 → ops + 实例上下文」登记下来；
 *    - 任何路径请求先做「最长前缀匹配」，再转发给对应的驱动。
 *      例：/mnt/usb/photo.jpg 命中挂在 /mnt/usb 的 FAT32；
 *          其余路径命中挂在 / 的 SRFS。
 *
 *  兼容性：fs_xxx()（fs.h）函数名与签名完全保留，内部改为经本层
 *  转发（fs_listdir/fs_stat/fs_read/fs_write）。SRFS 专有的
 *  create/remove/chdir/statfs/format 等仍直接落在 fs.c，
 *  因为不同文件系统的「创建/删除/挂载」语义并不通用。
 *
 *  无 malloc：挂载实例由各驱动持有静态单例，mount() 把指针写回 *ctx。
 * ============================================================ */

#define VFS_MAX_MOUNTS  16

struct vfs_mount_info {
    char     path[FS_PATH_MAX];   /* 挂载点 */
    char     source[32];          /* 块设备名，如 "disk0p1" */
    char     fs_name[16];         /* "srfs" */
    bool     is_root;
};

/* 统一的文件系统操作表。
 * ctx 是该文件系统的「实例上下文」（SRFS 为 NULL；FAT32 指向其静态
 * 挂载实例）。path 一律为调用方原始路径；驱动自行解析相对路径。 */
struct filesystem_ops {
    const char *name;                  /* "srfs" / "fat32" …（调试用） */

    /* 挂载 source（设备/分区标识，可为 NULL）。成功返回 FS_OK 并置 *ctx。 */
    int  (*mount)  (const char *source, void **ctx);
    void (*umount) (void *ctx);

    int  (*listdir)(void *ctx, const char *path, struct fs_dirent *out, int max);
    int  (*stat)   (void *ctx, const char *path, struct fs_dirent *out);
    int  (*read)   (void *ctx, const char *path, void *buf,
                    uint64_t count, uint64_t off);
    int  (*write)  (void *ctx, const char *path, const void *buf,
                    uint64_t count, uint64_t off);
};

/* ---- 挂载表 ---- */
void vfs_reset(void);                       /* 清空挂载表（重新挂载前调用） */

/* 把 ops 挂到 mountpoint（如 "/"、"/mnt/usb"）。
 * 成功返回 FS_OK；挂载点已占用 FS_ERR_EXIST；无空槽 FS_ERR_NOSPACE；
 * 驱动 mount() 失败则返回其错误码。 */
int  vfs_mount(const char *mountpoint, const struct filesystem_ops *ops,
               const char *source);
int  vfs_umount(const char *mountpoint);

/* 精确查询某个挂载点是否存在（不做前缀匹配）。 */
bool vfs_is_mounted(const char *mountpoint);

/* 枚举挂载表（GUI 驱动器列表用）。返回写入 out 的条目数。 */
int  vfs_mount_count(void);
int  vfs_list_mounts(struct vfs_mount_info *out, int max);

/* 最长前缀匹配：命中返回 ops 并写 *ctx（可为 NULL），否则返回 NULL。
 * mount_len（可为 NULL）返回命中的挂载点长度（"/" 为 1）。
 * 相对路径一律按根挂载处理。
 * 注意：vfs_listdir/stat/read/write 转发给驱动的 path 已去掉挂载前缀，
 * 例如挂在 /mnt/usb 的驱动收到 "/photo.jpg"，挂载点本身收到 "/"。 */
const struct filesystem_ops *vfs_route(const char *path, void **ctx,
                                       size_t *mount_len);

/* 去掉挂载前缀后的驱动视角路径（见 vfs_route 说明）。 */
const char *vfs_subpath(const char *path, size_t mount_len);

/* ---- 通用转发（fs_xxx 内部走这里） ---- */
int  vfs_listdir(const char *path, struct fs_dirent *out, int max);
int  vfs_stat(const char *path, struct fs_dirent *out);
int  vfs_read(const char *path, void *buf, uint64_t count, uint64_t off);
int  vfs_write(const char *path, const void *buf, uint64_t count, uint64_t off);

#endif
