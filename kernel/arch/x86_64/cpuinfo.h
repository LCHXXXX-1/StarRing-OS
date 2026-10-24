#ifndef CPUINFO_H
#define CPUINFO_H
#include <stdint.h>
#include <stdbool.h>

struct cpu_info {
    char     vendor[13];
    char     brand[49];
    uint32_t family, model, stepping;
    uint32_t logical_cores, physical_cores;
    bool has_apic, has_x2apic, has_sse, has_sse2, has_avx, has_avx2;
};
void cpuinfo_init(void);
const struct cpu_info *cpuinfo_get(void);
#endif
