#ifndef SYSINFO_H
#define SYSINFO_H
#include "arch/x86_64/cpuinfo.h"
#include "hw/meminfo.h"
const struct cpu_info *sysinfo_cpu(void);
const struct mem_info *sysinfo_mem(void);
#endif
