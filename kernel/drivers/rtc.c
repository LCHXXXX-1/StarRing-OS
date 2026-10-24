#include "rtc.h"
#include "arch/x86_64/cpu.h"
#include "drivers/serial.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

#define REG_SEC   0x00
#define REG_MIN   0x02
#define REG_HOUR  0x04
#define REG_DAY   0x07
#define REG_MON   0x08
#define REG_YEAR  0x09
#define REG_STATC 0x0C
#define REG_STATB 0x0B

/* ============================================================
 *  CMOS 读取的三道保险（老主板 / Legacy BIOS 上必需）
 *   1) 每一次“等硬件状态”的循环都有次数上限（RTC_WAIT_MAX），
 *      绝不无限死等；
 *   2) 读之前置 REG_STATB.SET=1（冻结更新 + 屏蔽 NMI），读完清 0，
 *      避免读到跨秒进位的撕裂值、也避免老南桥发 NMI 打乱启动；
 *   3) 一次超时即判定这块 CMOS 不可靠（rtc_dead），后续调用直接
 *      返回 false，不再反复自旋 —— 任务栏时钟每 0.5s 也要读，
 *      没有这个闩锁会把 CPU 全烧在 io 自旋上。
 * ============================================================ */

#define RTC_WAIT_MAX 100000u    /* 等 UIP 清零的最大轮询次数 */
#define RTC_RETRY    3u         /* 整组寄存器的最大重试次数 */

static volatile int rtc_dead = 0;

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg & 0x7F);    /* bit7=0：不清 NMI flag */
    io_delay();
    uint8_t v = inb(CMOS_DATA);
    io_delay();
    return v;
}

static void cmos_write(uint8_t reg, uint8_t val) {
    outb(CMOS_ADDR, reg & 0x7F);
    io_delay();
    outb(CMOS_DATA, val);
    io_delay();
}

/* 等上一次更新完成（STATUS_A 位7 = UIP）。
   带次数上限：超时返回 false（并闩锁 rtc_dead），由调用方放弃读取。 */
static bool rtc_wait_ready(void) {
    if (rtc_dead) return false;
    for (uint32_t i = 0; i < RTC_WAIT_MAX; i++) {
        if ((cmos_read(0x0A) & 0x80) == 0)
            return true;            /* 空闲，可以读 */
    }
    serial_puts("[SRCore] rtc: UIP timeout, clock ignored\n");
    rtc_dead = 1;                   /* 反复超时 -> 认定 CMOS 不可用 */
    return false;
}

/* 冻结 RTC 更新后读一组寄存器；unfreeze 由调用方配对完成 */
static void rtc_freeze(bool on) {
    if (rtc_dead) return;
    uint8_t b = cmos_read(REG_STATB);
    if (on) { if (!(b & 0x80)) cmos_write(REG_STATB, b | 0x80); }
    else    { if (b & 0x80)   cmos_write(REG_STATB, b & (uint8_t)~0x80); }
}

static int bcd2bin(uint8_t v) { return (v & 0x0F) + ((v >> 4) * 10); }

/* 十进制模式下 BCD 残留的坏值也在这里挡掉（老 CMOS 常见 0x0A..0x0F） */
static bool time_sane(int h, int m, int s) {
    return h >= 0 && h <= 23 && m >= 0 && m <= 59 && s >= 0 && s <= 59;
}

bool rtc_read(int *hour, int *min, int *sec) {
    if (!hour || !min || !sec) return false;
    if (rtc_dead) return false;

    uint8_t st;
    uint8_t s, m, h;
    bool  got = false;

    st = cmos_read(REG_STATB);
    rtc_freeze(true);                /* SET=1：停止更新 */

    for (uint32_t retry = 0; retry < RTC_RETRY && !got; retry++) {
        if (!rtc_wait_ready()) break;

        s = cmos_read(REG_SEC);
        m = cmos_read(REG_MIN);
        h = cmos_read(REG_HOUR);

        if (cmos_read(0x0A) & 0x80) continue;   /* 读的过程中又开始了更新 */

        int is_bcd = !(st & 0x04);
        int h24    = (st & 0x02) != 0;

        int si = is_bcd ? bcd2bin(s) : s;
        int mi = is_bcd ? bcd2bin(m) : m;
        int hi, pm;
        if (is_bcd) { pm = (h & 0x80) != 0; hi = bcd2bin(h & 0x7F); }
        else        { pm = (h & 0x80) != 0; hi = h & 0x7F; }

        if (!h24) {                          /* 12 小时制折算成 24 小时 */
            if (pm && hi < 12) hi += 12;
            if (!pm && hi == 12) hi = 0;
        }

        if (si > 59 || mi > 59 || hi > 23 || !time_sane(hi, mi, si))
            continue;                        /* 数据不合理，再试一次 */

        *hour = hi; *min = mi; *sec = si;
        got = true;
    }

    rtc_freeze(false);                       /* SET=0：恢复更新 */
    return got;
}

bool rtc_read_date(int *day, int *month, int *year) {
    if (!day || !month || !year) return false;
    if (rtc_dead) return false;

    uint8_t st = cmos_read(REG_STATB);
    rtc_freeze(true);

    int di = 0, mi = 0, yi = 0;
    bool got = false;

    for (uint32_t retry = 0; retry < RTC_RETRY && !got; retry++) {
        if (!rtc_wait_ready()) break;

        uint8_t d = cmos_read(REG_DAY);
        uint8_t m = cmos_read(REG_MON);
        uint8_t y = cmos_read(REG_YEAR);

        int is_bcd = !(st & 0x04);
        di = is_bcd ? bcd2bin(d) : d;
        mi = is_bcd ? bcd2bin(m) : m;
        yi = is_bcd ? bcd2bin(y) : y;
        yi += (yi < 70) ? 2000 : 1900;       /* CMOS 只存两位年份 */

        if (di < 1 || di > 31 || mi < 1 || mi > 12) continue;
        got = true;
    }

    rtc_freeze(false);
    if (!got) return false;
    *day = di; *month = mi; *year = yi;
    return true;
}

/* RTC 是否已经确认不可用（状态机/上层可以用来决定是否显示时钟） */
bool rtc_dead_locked(void) { return rtc_dead != 0; }
