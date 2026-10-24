#include "sysinfo.h"
const struct cpu_info *sysinfo_cpu(void) { return cpuinfo_get(); }
const struct mem_info *sysinfo_mem(void) { return meminfo_get(); }
