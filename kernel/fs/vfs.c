#include "vfs.h"
#include "lib/string.h"

/* ============================================================
 *  VFS 挂载路由层实现（阶段二）
 *
 *  挂载表是静态单实例（无 malloc）。路径路由用「最长前缀匹配」，
 *  保证 /mnt/usb 这类嵌套挂载点优先于根挂载 "/"。
 * ============================================================ */

struct vfs_mount {
    char                         path[FS_PATH_MAX];   /* 规范化后："/" 或 "/mnt/disk1p1" */
    char                         source[32];          /* block device / 设备标识 */
    const struct filesystem_ops *ops;
    void                        *ctx;
    bool                         used;
};

static struct vfs_mount mounts[VFS_MAX_MOUNTS];

void vfs_reset(void) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].used && mounts[i].ops && mounts[i].ops->umount)
            mounts[i].ops->umount(mounts[i].ctx);
        mounts[i].used    = false;
        mounts[i].ops     = 0;
        mounts[i].ctx     = 0;
        mounts[i].path[0] = 0;
        mounts[i].source[0] = 0;
    }
}

/* 规范化挂载点：保证以 '/' 开头、折叠重复斜杠、去掉结尾斜杠
   （根 "/" 除外）。空串 → "/"。写入 out（有界）。 */
static void norm_mount(const char *in, char *out, size_t outsz) {
    if (!in) in = "/";
    if (outsz < 2) { if (outsz) out[0] = 0; return; }

    size_t n = 0;
    if (in[0] != '/') out[n++] = '/';       /* 补前导 '/' */

    for (const char *p = in; *p && n + 1 < outsz; p++) {
        if (*p == '/') {
            if (n == 0) { out[n++] = '/'; continue; }   /* 补前导 '/' */
            if (out[n - 1] == '/') continue;            /* 折叠重复斜杠 */
            out[n++] = '/';
        } else {
            out[n++] = *p;
        }
    }
    if (n == 0) out[n++] = '/';
    while (n > 1 && out[n - 1] == '/') n--;             /* 去结尾斜杠 */
    out[n] = 0;
}

/* path 是否落在挂载点 mount 之下（mount 已规范化，无结尾斜杠）。
   "/" 匹配一切绝对路径；"/mnt/usb" 匹配 "/mnt/usb" 与 "/mnt/usb/..."。 */
static bool under_mount(const char *mount, const char *path) {
    if (mount[0] == '/' && mount[1] == 0) return path[0] == '/';
    size_t ml = kstrlen(mount);
    for (size_t i = 0; i < ml; i++)
        if (path[i] != mount[i]) return false;
    return path[ml] == 0 || path[ml] == '/';
}

int vfs_mount(const char *mountpoint, const struct filesystem_ops *ops,
              const char *source) {
    if (!ops) return FS_ERR_INVAL;

    char mp[FS_PATH_MAX];
    norm_mount(mountpoint, mp, sizeof(mp));

    /* 挂载点已占用？ */
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mounts[i].used && kstrcmp(mounts[i].path, mp) == 0)
            return FS_ERR_EXIST;

    /* 找空槽 */
    int slot = -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (!mounts[i].used) { slot = i; break; }
    if (slot < 0) return FS_ERR_NOSPACE;

    /* 让驱动初始化自己的实例上下文（可选） */
    void *ctx = 0;
    if (ops->mount) {
        int r = ops->mount(source, &ctx);
        if (r != FS_OK) return r;
    }

    kstrcpy(mounts[slot].path, mp);
    mounts[slot].source[0] = 0;
    if (source) {
        size_t n = 0;
        while (source[n] && n + 1 < sizeof(mounts[slot].source)) {
            mounts[slot].source[n] = source[n];
            n++;
        }
        mounts[slot].source[n] = 0;
    }
    mounts[slot].ops  = ops;
    mounts[slot].ctx  = ctx;
    mounts[slot].used = true;
    return FS_OK;
}

int vfs_umount(const char *mountpoint) {
    char mp[FS_PATH_MAX];
    norm_mount(mountpoint, mp, sizeof(mp));

    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].used || kstrcmp(mounts[i].path, mp) != 0) continue;
        if (mounts[i].ops && mounts[i].ops->umount)
            mounts[i].ops->umount(mounts[i].ctx);
        mounts[i].used    = false;
        mounts[i].ops     = 0;
        mounts[i].ctx     = 0;
        mounts[i].path[0] = 0;
        mounts[i].source[0] = 0;
        return FS_OK;
    }
    return FS_ERR_NOENT;
}

bool vfs_is_mounted(const char *mountpoint) {
    char mp[FS_PATH_MAX];
    norm_mount(mountpoint, mp, sizeof(mp));
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mounts[i].used && mounts[i].ops &&
            kstrcmp(mounts[i].path, mp) == 0)
            return true;
    return false;
}

int vfs_mount_count(void) {
    int n = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mounts[i].used && mounts[i].ops) n++;
    return n;
}

int vfs_list_mounts(struct vfs_mount_info *out, int max) {
    if (!out || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS && n < max; i++) {
        if (!mounts[i].used || !mounts[i].ops) continue;
        kstrcpy(out[n].path, mounts[i].path);
        kstrcpy(out[n].source, mounts[i].source);
        kstrcpy(out[n].fs_name, mounts[i].ops->name ? mounts[i].ops->name : "");
        out[n].is_root = (mounts[i].path[0] == '/' && mounts[i].path[1] == 0);
        n++;
    }
    return n;
}

const struct filesystem_ops *vfs_route(const char *path, void **ctx,
                                       size_t *mount_len) {
    if (!path) path = "/";
    if (path[0] != '/') path = "/";     /* 相对路径按根挂载处理 */
    if (mount_len) *mount_len = 1;

    const struct filesystem_ops *best = 0;
    size_t best_len = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].used || !mounts[i].ops) continue;
        if (!under_mount(mounts[i].path, path)) continue;
        size_t l = kstrlen(mounts[i].path);
        if (!best || l > best_len) {     /* 最长前缀优先 */
            best     = mounts[i].ops;
            best_len = l;
            if (ctx) *ctx = mounts[i].ctx;
            if (mount_len) *mount_len = l;
        }
    }
    return best;
}

/* 驱动视角的路径：去掉挂载前缀（根挂载 "/" 原样保留前导 '/'）。
   "/mnt/usb/a" 在 /mnt/usb 上 → "/a"；挂载点本身 → "/"。 */
const char *vfs_subpath(const char *path, size_t mount_len) {
    if (!path) return "/";
    if (path[0] != '/') return path;
    if (mount_len <= 1) return path;
    const char *sub = path + mount_len;
    return *sub ? sub : "/";
}

/* ---------------- 通用转发 ---------------- */

int vfs_listdir(const char *path, struct fs_dirent *out, int max) {
    if (!path) path = "/";
    void *ctx = 0;
    size_t ml = 1;
    const struct filesystem_ops *ops = vfs_route(path, &ctx, &ml);
    if (!ops || !ops->listdir) return FS_ERR_NOENT;
    return ops->listdir(ctx, vfs_subpath(path, ml), out, max);
}

int vfs_stat(const char *path, struct fs_dirent *out) {
    if (!path) path = "/";
    void *ctx = 0;
    size_t ml = 1;
    const struct filesystem_ops *ops = vfs_route(path, &ctx, &ml);
    if (!ops || !ops->stat) return FS_ERR_NOENT;
    return ops->stat(ctx, vfs_subpath(path, ml), out);
}

int vfs_read(const char *path, void *buf, uint64_t count, uint64_t off) {
    if (!path) path = "/";
    void *ctx = 0;
    size_t ml = 1;
    const struct filesystem_ops *ops = vfs_route(path, &ctx, &ml);
    if (!ops || !ops->read) return FS_ERR_NOENT;
    return ops->read(ctx, vfs_subpath(path, ml), buf, count, off);
}

int vfs_write(const char *path, const void *buf, uint64_t count, uint64_t off) {
    if (!path) path = "/";
    void *ctx = 0;
    size_t ml = 1;
    const struct filesystem_ops *ops = vfs_route(path, &ctx, &ml);
    if (!ops || !ops->write) return FS_ERR_NOENT;
    return ops->write(ctx, vfs_subpath(path, ml), buf, count, off);
}
