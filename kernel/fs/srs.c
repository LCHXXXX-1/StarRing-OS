#include "srs.h"
#include "fs.h"
#include "lib/string.h"

/* ============================================================
 *  .srs 解析：整份文件读进静态缓冲后逐行扫描，不做任何堆分配。
 *  只认 key=value；等号两侧空白、行尾 \r、成对引号都会被去掉。
 * ============================================================ */

static char srs_buf[SRS_MAX_BYTES];

static bool key_is(const char *p, int len, const char *key) {
    int n = 0;
    while (n < len && key[n] && p[n] == key[n]) n++;
    return n == len && key[n] == 0;
}

bool srs_get(const char *file, const char *key, char *out, int out_max) {
    if (out && out_max > 0) out[0] = 0;
    if (!file || !key || !key[0] || !out || out_max <= 1) return false;
    if (!fs_ready()) return false;

    struct fs_dirent st;
    if (fs_stat(file, &st) < 0 || st.size == 0) return false;

    uint64_t want = st.size;
    if (want > (uint64_t)(SRS_MAX_BYTES - 1)) want = SRS_MAX_BYTES - 1;
    int n = fs_read(file, srs_buf, want, 0);
    if (n <= 0) return false;
    srs_buf[n] = 0;

    const char *p = srs_buf;
    while (*p) {
        const char *eol = p;
        while (*eol && *eol != '\n') eol++;

        const char *s = p, *e = eol;
        while (s < e && (*s == ' ' || *s == '\t' || *s == '\r')) s++;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;

        if (e > s && *s != '#' && *s != ';') {
            const char *eq = s;
            while (eq < e && *eq != '=') eq++;
            if (eq < e) {
                const char *k1 = s, *k2 = eq;
                while (k2 > k1 && (k2[-1] == ' ' || k2[-1] == '\t')) k2--;
                const char *v1 = eq + 1, *v2 = e;
                while (v1 < v2 && (*v1 == ' ' || *v1 == '\t')) v1++;
                if (v2 > v1 + 1 && (*v1 == '"' || *v1 == '\'') && v2[-1] == *v1) {
                    v1++; v2--;                        /* 去掉成对引号 */
                }
                if (key_is(k1, (int)(k2 - k1), key)) {
                    int vl = (int)(v2 - v1);
                    if (vl > out_max - 1) vl = out_max - 1;
                    for (int i = 0; i < vl; i++) out[i] = v1[i];
                    out[vl] = 0;
                    return true;
                }
            }
        }
        p = (*eol == '\n') ? eol + 1 : eol;
    }
    return false;
}
