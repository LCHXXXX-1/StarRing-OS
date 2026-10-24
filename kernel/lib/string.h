#ifndef STRING_H
#define STRING_H
#include <stdint.h>
#include <stddef.h>
char *itoa(int64_t value, char *buf, int base);
char *utoa(uint64_t value, char *buf, int base);
size_t kstrlen(const char *s);
int    kstrcmp(const char *a, const char *b);
char  *kstrcpy(char *dst, const char *src);
#endif
