#ifndef HEAP_H
#define HEAP_H
#include <stdint.h>
#include <stdbool.h>

bool     heap_init(void);
void    *_kmalloc(uint64_t size, const char *file, int line);
void     kfree(void *ptr);
/* 已分配块的实际可用字节数（不含 32 字节块头）；非法指针返回 0。
   给 realloc 语义的垫片用：只能拷贝 min(旧容量, 新容量) 字节。 */
uint64_t kheap_usable_size(void *ptr);
uint64_t heap_total(void);
uint64_t heap_used(void);
uint64_t heap_free(void);

#define kmalloc(sz) _kmalloc((uint64_t)(sz), __FILE__, __LINE__)
#endif