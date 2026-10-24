#ifndef POWER_H
#define POWER_H

/* 关机 / 重启。两者都不返回。
   关机：ACPI PM1a_CNT 写 SLP_EN（QEMU/Bochs/VBox 都支持）；
   重启：先脉冲 i8042 复位线，失败再用三重故障兜底。 */
void power_shutdown(void);
void power_reboot(void);

#endif
