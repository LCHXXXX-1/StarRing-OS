#ifndef DISK_UTILITY_H
#define DISK_UTILITY_H

/* 磁盘工具（Disk Utility）：分区列表 + 可视化格式化。
   - 主界面：物理磁盘信息 + MBR 分区列表（# / 起始 LBA / 大小 / 类型 / 状态），
     点击或用 Up/Down 选择分区；底部 [Format Selected Partition] / [Cancel]
   - 安全防呆：选中当前根文件系统所在分区时，格式化按钮禁用，并显示红字
     "Cannot format the active system partition!"
   - 二次确认：[Yes, Erase] 才会真的调用 fs_format_disk_ex(start_lba, ...)
   - 格式化：真实写盘进度条（每写一个元数据块回调一次，FLUSH 后到 100%；
     在桌面主循环里跑，不在中断里跑）
   - 只格式化选中分区，绝不传 0 覆盖 MBR；格式化非活动分区后当前系统仍可用
   - 键盘：Up/Down 选分区，Left/Right 切换按钮焦点，Space/Enter 激活，
     Esc 返回/关闭，确认页按 Y 直接擦除 */
void disk_utility_open(void);

#endif
