#include "lib/string.h"
#include <stdbool.h>

char *itoa(int64_t value, char *buf, int base) {
    char tmp[65]; int i = 0; bool neg = false;
    if (value < 0 && base == 10) { neg = true; value = -value; }
    if (value == 0) tmp[i++] = '0';
    while (value) {
        int d = value % base;
        tmp[i++] = (d < 10) ? ('0' + d) : ('a' + d - 10);
        value /= base;
    }
    if (neg) tmp[i++] = '-';
    int j = 0;
    while (i) buf[j++] = tmp[--i];
    buf[j] = 0;
    return buf;
}
char *utoa(uint64_t value, char *buf, int base) {
    char tmp[65]; int i = 0;
    if (value == 0) tmp[i++] = '0';
    while (value) {
        int d = value % base;
        tmp[i++] = (d < 10) ? ('0' + d) : ('a' + d - 10);
        value /= base;
    }
    int j = 0;
    while (i) buf[j++] = tmp[--i];
    buf[j] = 0;
    return buf;
}
size_t kstrlen(const char *s) { size_t n = 0; while (*s++) n++; return n; }
int kstrcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
char *kstrcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) ;
    return dst;
}
