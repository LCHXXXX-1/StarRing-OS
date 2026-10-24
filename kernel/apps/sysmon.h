#ifndef SYSMON_H
#define SYSMON_H

/* 系统监视器（内存/分页）：展示 PMM 物理页帧统计与 VMM 分页状态，
   并提供一个“分页自检”按钮：分配页帧 → 建立映射 → 写入并读回 → 解除映射。 */
void sysmon_open(void);

#endif
