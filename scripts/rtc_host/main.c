// scripts/rtc_host/main.c — deterministic RTC decode + straddle cases.
//
// Compiled together with the SHIPPED src/drivers/rtc.c and the fake chip, and
// driven by scripts/rtc_decode_test.py. Every case goes through the driver's
// public entry point (rtc_read_time), which is why the same harness is RED on
// the v38.164 driver and GREEN on the fixed one:
//
//   control            BCD + 24-hour      both versions agree (the sanity check)
//   binary mode        Status B DM=1      pre-fix applies BCD unconditionally
//   twelve-hour PM     hour bit 7 = PM    pre-fix reads 9 where the answer is 21
//   twelve-hour 12 AM  hour == 12, no PM  pre-fix reads 12 where the answer is 0
//   straddle           update mid-burst   pre-fix returns a MIXTURE of the two
//                                         images; the fix retries and returns
//                                         the post-update image whole
//
// The straddle case also asserts the positive outcome: a retry happened, and
// the driver's "no stable stamp" line was NOT written — recovering by retry is
// not the same as recovering by best-effort, and only the first is a pass.
// The kernel headers come FIRST on purpose: src/include/types.h defines NULL
// unconditionally, and a libc header (stdio.h, string.h) included before it
// would trigger a "NULL redefined" warning — this harness builds with the
// project's zero-warning rule.
#include "rtc.h"
#include "io.h"
#include "serial.h"

#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

static void reset_chip(void) {
    memset(rtc_host_regs, 0, sizeof(rtc_host_regs));
    memset(rtc_host_pending, 0, sizeof(rtc_host_pending));
    rtc_host_index = 0;
    rtc_host_data_reads = 0;
    rtc_host_reg_a_reads = 0;
    rtc_host_port70_writes = 0;
    rtc_host_swap_after = -1;
    rtc_host_uip_on_reg_a_read = -1;
    rtc_host_serial_len = 0;
    rtc_host_serial[0] = '\0';
}

static void reg(unsigned char idx, unsigned char v) { rtc_host_regs[idx] = v; }

static void expect(int cond, const char* what) {
    checks++;
    if (cond) {
        printf("[OK] %s\n", what);
    } else {
        failures++;
        printf("[FAIL] %s\n", what);
    }
}

static int eq(const rtc_time_t* t, int s, int mi, int h, int d, int mo, int y, int dow) {
    return t->second == s && t->minute == mi && t->hour == h &&
           t->day == d && t->month == mo && t->year == (unsigned)y &&
           t->dow == dow;
}

static void show(const char* label, const rtc_time_t* t) {
    printf("       %s = %02u:%02u:%02u %02u/%02u/%u dow=%u\n", label,
           t->hour, t->minute, t->second, t->day, t->month, t->year, t->dow);
}

// BCD layout used by the images below (Status B is register 0x0B).
#define B_24H_BCD 0x02
#define B_24H_BIN 0x06   // 24-hour + DM (binary)
#define B_12H_BCD 0x00   // 12-hour + BCD

int main(void) {
    rtc_time_t t;

    // ---- 1. Control: BCD + 24-hour. 2007-06-05 03:22:11 -------------------
    reset_chip();
    reg(0x00, 0x11); reg(0x02, 0x22); reg(0x04, 0x03); reg(0x06, 0x01);
    reg(0x07, 0x05); reg(0x08, 0x06); reg(0x09, 0x07); reg(0x32, 0x20);
    reg(0x0B, B_24H_BCD);
    t = rtc_read_time();
    show("control", &t);
    expect(eq(&t, 11, 22, 3, 5, 6, 2007, 1),
           "BCD/24h: a plain image decodes to 2007-06-05 03:22:11");

    // ---- 2. Binary mode: DM=1 means the bytes are NOT BCD ----------------
    // 2026-02-28 23:07:45, written as binary values.
    reset_chip();
    reg(0x00, 45); reg(0x02, 7); reg(0x04, 23); reg(0x06, 3);
    reg(0x07, 28); reg(0x08, 2); reg(0x09, 26); reg(0x32, 20);
    reg(0x0B, B_24H_BIN);
    t = rtc_read_time();
    show("binary", &t);
    expect(eq(&t, 45, 7, 23, 28, 2, 2026, 3),
           "binary mode (Status B DM=1): the bytes are taken as binary, "
           "not BCD");

    // ---- 3. Twelve-hour mode, PM: hour bit 7 is the PM flag --------------
    // 2026-10-09 21:07:05 is stored as BCD 09 + PM.
    reset_chip();
    reg(0x00, 0x05); reg(0x02, 0x07); reg(0x04, 0x80 | 0x09); reg(0x06, 0x04);
    reg(0x07, 0x09); reg(0x08, 0x10); reg(0x09, 0x26); reg(0x32, 0x20);
    reg(0x0B, B_12H_BCD);
    t = rtc_read_time();
    show("12h pm", &t);
    expect(eq(&t, 5, 7, 21, 9, 10, 2026, 4),
           "12-hour PM: 0x89 decodes to 21, not 9");

    // ---- 4. Twelve-hour mode, 12 AM: hour 12 with no PM flag is 00 -------
    reset_chip();
    reg(0x00, 0x00); reg(0x02, 0x30); reg(0x04, 0x12); reg(0x06, 0x05);
    reg(0x07, 0x01); reg(0x08, 0x01); reg(0x09, 0x26); reg(0x32, 0x20);
    reg(0x0B, B_12H_BCD);
    t = rtc_read_time();
    show("12h am", &t);
    expect(eq(&t, 0, 30, 0, 1, 1, 2026, 5),
           "12-hour 12 AM: hour 12 without the PM flag decodes to 0");

    // ---- 5. Straddle: the update lands inside the burst -------------------
    // The old image is 2007-06-05 03:22:11 and the new one is
    // 2026-11-16 21:44:33. The swap happens after the 6th port read, so a
    // driver that reads the burst without bracketing it returns mixed fields;
    // the fix sees UIP on the closing Reg A read, retries, and returns the
    // new image entire.
    reset_chip();
    reg(0x00, 0x11); reg(0x02, 0x22); reg(0x04, 0x03); reg(0x06, 0x01);
    reg(0x07, 0x05); reg(0x08, 0x06); reg(0x09, 0x07); reg(0x32, 0x20);
    reg(0x0B, B_24H_BCD);

    rtc_host_pending[0x00] = 0x33;   // 33s
    rtc_host_pending[0x02] = 0x44;   // 44min
    rtc_host_pending[0x04] = 0x21;   // 21h  (BCD, 24-hour)
    rtc_host_pending[0x06] = 0x02;   // dow 2
    rtc_host_pending[0x07] = 0x16;   // 16th
    rtc_host_pending[0x08] = 0x11;   // November
    rtc_host_pending[0x09] = 0x26;   // 2026
    rtc_host_pending[0x32] = 0x20;   // century 20
    rtc_host_pending[0x0B] = B_24H_BCD;

    rtc_host_swap_after = 6;
    rtc_host_uip_on_reg_a_read = 3;   // the burst's closing Reg A read

    t = rtc_read_time();
    show("straddle", &t);
    expect(eq(&t, 33, 44, 21, 16, 11, 2026, 2),
           "straddle: an update inside the burst yields the post-update image "
           "whole, not a mixture");
    expect(!eq(&t, 11, 22, 3, 5, 6, 2007, 1),
           "straddle: no field of the pre-update image survives");
    expect(rtc_host_reg_a_reads >= 3,
           "straddle: the driver retried (Reg A bracketed the burst)");
    expect(strstr(rtc_host_serial, "no stable stamp") == NULL,
           "straddle: it recovered by retrying, not by best-effort");

    printf("rtc decode checks=%d failures=%d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
