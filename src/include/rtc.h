#ifndef RTC_H
#define RTC_H

#include "types.h"

typedef struct {
    unsigned char second;
    unsigned char minute;
    unsigned char hour;
    unsigned char day;
    unsigned char month;
    unsigned int  year;   // Full year (e.g., 2026)
    unsigned char dow;    // Day of week (1=Sunday..7=Saturday)
} rtc_time_t;

// ---- Raw register image (v38.165) --------------------------------------
//
// rtc_decode_regs() takes the CMOS bytes in the order the driver reads them
// (index, then value), so the decode rules — BCD vs binary from Status B's
// DM bit, 12- vs 24-hour from its 24/12 bit — can be exercised on the host
// without a chip: scripts/rtc_decode_test.py compiles src/drivers/rtc.c
// against scripts/rtc_host/ (fake ports, no-op locks) and drives these
// images. Keeping the decode a pure function of the image is what makes the
// straddle/binary/12-hour rules testable at all; the port pair itself can
// only be tested on real SMP, which apps/rtc_stress.c does.
enum {
    RTC_R_SEC = 0,      // 0x00
    RTC_R_MIN,          // 0x02
    RTC_R_HOUR,         // 0x04
    RTC_R_DOW,          // 0x06
    RTC_R_DAY,          // 0x07
    RTC_R_MON,          // 0x08
    RTC_R_YEAR,         // 0x09
    RTC_R_CENTURY,      // 0x32
    RTC_R_STATUS_B,     // 0x0B
    RTC_R_COUNT
};

// Decode such an image into a calendar time. Returns 0 on success and -1 when
// the image cannot be a calendar time — a crossed read, an update caught
// mid-burst, or a wedged RTC — in which case *out is left as the decode of
// the same bytes (callers log and retry; nothing is silently invented).
int rtc_decode_regs(const unsigned char* regs, rtc_time_t* out);

// Initialize RTC: probes the century register (and its format) once.
void rtc_init(void);

// Read current time from CMOS RTC. The 0x70/0x71 index/value pair is a single
// locked transaction, and a burst that an update lands inside is retried.
rtc_time_t rtc_read_time(void);

#endif
