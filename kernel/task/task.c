#include "task.h"
#include "gfx/gui.h"
#include "drivers/timer.h"
#include "arch/x86_64/cpu.h"
#include "lib/string.h"

static struct task_info tasks[TASK_MAX];
static int      next_pid = 1;

/* 任务名有界拷贝（窗口标题最长 47，这里必须截断，绝不能越界） */
static void copy_name(char *dst, const char *src) {
    int i = 0;
    if (src) {
        for (; src[i] && i < TASK_NAME_MAX - 1; i++) dst[i] = src[i];
    }
    dst[i] = 0;
}

void task_init(void) {
    for (int i = 0; i < TASK_MAX; i++) tasks[i].used = false;
    next_pid = 1;
}

int task_add(const char *name, int kind, void *win) {
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].used) continue;
        tasks[i].used = true;
        tasks[i].pid = next_pid++;
        tasks[i].kind = kind;
        tasks[i].win = win;
        tasks[i].start_tick = timer_ticks();
        copy_name(tasks[i].name, name ? name : "task");
        return tasks[i].pid;
    }
    return -1;
}

void task_remove_win(void *win) {
    if (!win) return;
    for (int i = 0; i < TASK_MAX; i++)
        if (tasks[i].used && tasks[i].win == win) tasks[i].used = false;
}

void task_rename_win(void *win, const char *name) {
    if (!win || !name) return;
    for (int i = 0; i < TASK_MAX; i++)
        if (tasks[i].used && tasks[i].win == win) {
            copy_name(tasks[i].name, name);
            return;
        }
}

int task_count(void) {
    int n = 0;
    for (int i = 0; i < TASK_MAX; i++) if (tasks[i].used) n++;
    return n;
}

/* 按 pid 升序拷贝；n 很小，用插入排序即可 */
int task_snapshot(struct task_info *out, int max) {
    int n = 0;
    for (int i = 0; i < TASK_MAX && n < max; i++) {
        if (!tasks[i].used) continue;
        int j = n - 1;
        while (j >= 0 && out[j].pid > tasks[i].pid) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = tasks[i];
        n++;
    }
    return n;
}

bool task_kill(int pid) {
    for (int i = 0; i < TASK_MAX; i++) {
        if (!tasks[i].used || tasks[i].pid != pid) continue;
        if (tasks[i].kind == TASK_KIND_SYSTEM) return false;   /* 系统任务不可结束 */

        void *win = tasks[i].win;
        tasks[i].used = false;
        if (win) gui_window_close((struct gui_window *)win);   /* 关窗会再触发 remove */
        return true;
    }
    return false;
}
