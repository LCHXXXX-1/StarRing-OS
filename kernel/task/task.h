#ifndef TASK_H
#define TASK_H
#include <stdint.h>
#include <stdbool.h>

/* ============================================================
 *  Task registry —— 任务登记表
 *
 *  这是一个“可观测”的任务表：内核在启动时登记系统任务，GUI 每开一个
 *  窗口就自动登记一个应用任务（win 指向该窗口），窗口关闭时自动注销。
 *  “任务管理器”读这张表来展示与结束任务。
 *
 *  注意：本 OS 目前是单线程事件循环 + 中断，任务表并非抢占式调度器；
 *  它记录的是“谁在运行（存在）”，而不是 CPU 时间片。
 * ============================================================ */

#define TASK_MAX      16
#define TASK_NAME_MAX 40

#define TASK_KIND_SYSTEM 0
#define TASK_KIND_APP    1

struct task_info {
    bool     used;
    int      pid;
    char     name[TASK_NAME_MAX];
    int      kind;
    void    *win;              /* 关联的 GUI 窗口指针，0 表示无 */
    uint64_t start_tick;       /* 登记时刻的 timer tick */
};

void task_init(void);

/* 登记任务，返回 pid（>=1）；失败返回 -1 */
int  task_add(const char *name, int kind, void *win);
/* 窗口销毁时按指针注销；标题变化时同步名字 */
void task_remove_win(void *win);
void task_rename_win(void *win, const char *name);

/* 拷贝当前任务快照（按 pid 升序），返回条数 */
int  task_count(void);
int  task_snapshot(struct task_info *out, int max);

/* 结束任务：应用任务关窗口；系统任务拒绝。成功返回 true */
bool task_kill(int pid);

#endif
