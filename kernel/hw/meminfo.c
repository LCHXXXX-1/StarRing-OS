#include "meminfo.h"

static struct mem_info info;

bool meminfo_init(struct limine_memmap_response *resp) {
    if (!resp) return false;
    info.entry_count = resp->entry_count;
    info.total_bytes = 0; info.usable_bytes = 0;
    for (uint64_t i = 0; i < resp->entry_count; i++) {
        struct limine_memmap_entry *e = resp->entries[i];
        info.total_bytes += e->length;
        if (e->type == LIMINE_MEMMAP_USABLE)
            info.usable_bytes += e->length;
    }
    return true;
}
const struct mem_info *meminfo_get(void) { return &info; }
