#include "cpuinfo.h"
#include "cpuid.h"
#include "lib/mem.h"

static struct cpu_info info;

void cpuinfo_init(void) {
    uint32_t eax, ebx, ecx, edx;

    cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    uint32_t max_leaf = eax;
    memcpy(&info.vendor[0], &ebx, 4);
    memcpy(&info.vendor[4], &edx, 4);
    memcpy(&info.vendor[8], &ecx, 4);
    info.vendor[12] = 0;

    if (max_leaf >= 1) {
        cpuid(1, 0, &eax, &ebx, &ecx, &edx);
        info.stepping      = eax & 0xF;
        info.model         = (eax >> 4) & 0xF;
        info.family        = (eax >> 8) & 0xF;
        info.logical_cores = (ebx >> 16) & 0xFF;
        info.has_apic   = (edx >> 9)  & 1;
        info.has_sse    = (edx >> 25) & 1;
        info.has_sse2   = (edx >> 26) & 1;
        info.has_x2apic = (ecx >> 21) & 1;
        info.has_avx    = (ecx >> 28) & 1;
    }
    if (max_leaf >= 7) {
        cpuid(7, 0, &eax, &ebx, &ecx, &edx);
        info.has_avx2 = (ebx >> 5) & 1;
    }

    cpuid(0x80000000, 0, &eax, &ebx, &ecx, &edx);
    uint32_t max_ext = eax;
    if (max_ext >= 0x80000004) {
        uint32_t *p = (uint32_t *)info.brand;
        for (int i = 0; i < 3; i++)
            cpuid(0x80000002 + i, 0, &p[i*4], &p[i*4+1], &p[i*4+2], &p[i*4+3]);
        info.brand[48] = 0;
        int start = 0;
        while (info.brand[start] == ' ') start++;
        if (start > 0) memcpy(info.brand, info.brand + start, 49 - start);
    } else {
        memcpy(info.brand, info.vendor, 13);
    }
    if (max_ext >= 0x80000008) {
        cpuid(0x80000008, 0, &eax, &ebx, &ecx, &edx);
        info.physical_cores = (ecx & 0xFF) + 1;
    } else {
        info.physical_cores = info.logical_cores;
    }
    if (info.logical_cores == 0) info.logical_cores = 1;
}
const struct cpu_info *cpuinfo_get(void) { return &info; }
