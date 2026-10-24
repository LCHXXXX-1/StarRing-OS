#ifndef FONT_TTF_H
#define FONT_TTF_H
#include <stdint.h>
#include <stdbool.h>

/* 唯一默认字体路径：所有模块都从这里取，不再各自写死文件名。
   实际路径优先取自 /etc/system.srs 的 font_path=...（见 srs.c）。 */
#define TTF_FONT_PATH_DEFAULT "/usr/share/fonts/siyuan.sfn"
#define TTF_FONT_CONF          "/etc/system.srs"

bool     ttf_init(const char *path);
/* 按 /etc/system.srs 的 font_path 初始化；文件/键不存在时回退默认路径。
   返回实际是否加载成功。 */
bool     ttf_init_default(void);
/* 直接把 Limine module 里的字体装进 SSFN（零拷贝）。
   无盘 / Live CD 用；module 内存由 bootloader 常驻，不释放。 */
bool     ttf_init_module(void);
/* 空盘补给：磁盘上没有字体文件时，把 module 里的 siyuan.sfn 写进 SRFS。
   幂等（已存在则跳过）；无盘返回 false，不影响后续 ttf_init_module()。 */
bool     font_provision(void);
void     ttf_shutdown(void);
bool     ttf_available(void);

uint32_t utf8_decode(const char **s);

void     ttf_draw_text(int32_t x, int32_t y, const char *str,
                       uint32_t fg, uint32_t bg);
uint32_t ttf_get_text_width(const char *str);
uint32_t ttf_line_height(void);
void     ttf_cache_clear(void);

/* 渲染自检：打印分辨率/帧缓冲几何，并对 ASCII 与常用汉字做 bbox +
   真实光栅化（数不透明像素）。用来区分「字体没加载」与「字形数据残/
   缺 CJK」——VMware 全白时先看这段串口。 */
void     ttf_selftest(void);

#endif