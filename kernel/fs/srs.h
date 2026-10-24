#ifndef FS_SRS_H
#define FS_SRS_H
#include <stdbool.h>

/* ============================================================
 *  .srs (StarRing Settings) 配置解析
 *  格式：每行 key=value；# 与 ; 开头为注释；空行忽略；
 *        等号两侧空白自动去掉；值可带成对的双/单引号；不支持续行。
 *  无 libc、无 malloc：整份文件读进静态缓冲后就地解析。
 * ============================================================ */

#define SRS_SYSTEM_PATH  "/etc/system.srs"
#define SRS_MAX_BYTES    1024u    /* 配置文件读取上限（静态缓冲，含结束符） */

/* 取 file 中 key 对应的值。找到返回 true，out 保证以 NUL 结尾。
   未挂载文件系统 / 文件不存在 / 键不存在 → 返回 false 且 out[0]=0。 */
bool srs_get(const char *file, const char *key, char *out, int out_max);

#endif
