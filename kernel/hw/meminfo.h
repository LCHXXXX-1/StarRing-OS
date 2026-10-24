#ifndef MEMINFO_H
#define MEMINFO_H
#include <stdint.h>
#include <stdbool.h>
#include <limine.h>

struct mem_info {
    uint64_t total_bytes, usable_bytes, entry_count;
};
bool meminfo_init(struct limine_memmap_response *resp);
const struct mem_info *meminfo_get(void);
#endif
