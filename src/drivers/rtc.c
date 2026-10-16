// ============================================================
// rtc.c — Mectov OS CMOS Real-Time Clock Driver
// ============================================================
// Reads time/date from the MC146818 RTC chip (CMOS).
//
// A CMOS register read is a TWO-PORT transaction: the register index goes to
// 0x70, then the value comes back from 0x71. Nothing in the chip binds those
// two accesses to each other, so with more than one CPU the pair must be
// serialized by software or the value read belongs to whichever register was
// selected LAST — another reader's. Four real readers exist (SYS_GET_TIME
// from any task on any core, the taskbar/login clocks, and the timer's
// once-per-second rate calibration), which is why this is a lock and not a
// comment: v38.165 measured 32k-37k torn stamps out of 48k samples with four
// threads of MIXED fields — an hour of 40, a month of 26, a year of 1530.
//
// on top of the pair, the chip must not be mid-update while the burst runs:
// Register A's UIP bit is set ~244us before the registers change, so the
// burst is bracketed by two Reg A reads and retried if either shows UIP.
//
// Status Register B is read in the same burst and honoured: bit 2 (DM)
// selects BINARY over BCD, and bit 1 (24/12) selects the 12-hour format whose
// hour bit 7 is a PM flag. The previous version applied BCD unconditionally,
// so an RTC in binary or 12-hour mode decoded to nonsense — invisible on
// QEMU/SeaBIOS (BCD + 24h), which is exactly why it is tested on the host.
//
// The decode is a pure function of the register image (rtc_decode_regs), so
// those two rules have a deterministic gate; the port pair can only be
// exercised on real SMP (apps/rtc_stress.c, scripts/rtc_test.py).
// ============================================================

#include "../include/rtc.h"
#include "../include/io.h"
#include "../include/spinlock.h"
#include "../include/serial.h"   // write_serial_* for the instability line

// CMOS I/O ports
#define CMOS_ADDR   0x70
#define CMOS_DATA   0x71

// Register addresses
#define RTC_SEC     0x00
#define RTC_MIN     0x02
#define RTC_HOUR    0x04
#define RTC_DOW     0x06   // Day of week (1=Sunday..7=Saturday)
#define RTC_DAY     0x07   // Day of month
#define RTC_MON     0x08
#define RTC_YEAR    0x09
#define RTC_CENTURY 0x32   // Optional, may not exist on all hardware

// Status Register A
#define RTC_REG_A   0x0A
#define RTC_UIP     0x80   // Update-In-Progress bit

// Status Register B
#define RTC_REG_B    0x0B
#define RTC_B_24H    0x02  // 1 = 24-hour mode, 0 = 12-hour (bit 7 of hour = PM)
#define RTC_B_DM     0x04  // 1 = binary values, 0 = BCD

// UIP clears within ~2 ms of the update; ~10 ms of bounded polling is far
// past that and still finite, so a wedged chip cannot hang a caller (same
// rule the serial and ATA paths follow for a dead port).
#define RTC_UIP_TRIES     10000
#define RTC_READ_ATTEMPTS 4

// NMI disable bit (bit 7 of address port)
#define NMI_DISABLE 0x80

static unsigned char century = 20; // Default: 2000s (kept as the fallback)

// Every reader of the index/value pair takes this: the pair is a single
// transaction on shared ports, and one CPU's inb(0x71) must never land
// between another CPU's outb(0x70) and its inb(0x71).
static spinlock_t rtc_lock = SPINLOCK_INIT;

// Read a CMOS register. The CALLER holds rtc_lock (house rule in
// spinlock.h: process context takes it with spin_lock_irqsave).
static inline unsigned char cmos_read_locked(unsigned char reg) {
    outb(CMOS_ADDR, NMI_DISABLE | reg);
    return inb(CMOS_DATA);
}

// Convert BCD to binary
static inline unsigned char bcd2bin(unsigned char bcd) {
    return ((bcd & 0xF0) >> 1) + ((bcd & 0xF0) >> 3) + (bcd & 0x0F);
}

// One field, in whichever format Status B selected. A value whose high nibble
// is not a decimal digit decodes above 100 and is caught by the plausibility
// check below, which is what makes a crossed read visible instead of silent.
static inline unsigned char field(unsigned char raw, int binary) {
    return binary ? raw : bcd2bin(raw);
}

int rtc_decode_regs(const unsigned char* regs, rtc_time_t* out) {
    const unsigned char sb = regs[RTC_R_STATUS_B];
    const int binary = (sb & RTC_B_DM) != 0;
    const int h12 = (sb & RTC_B_24H) == 0;

    out->second = field(regs[RTC_R_SEC], binary);
    out->minute = field(regs[RTC_R_MIN], binary);

    unsigned char raw_hour = regs[RTC_R_HOUR];
    int pm = 0;
    if (h12 && (raw_hour & 0x80)) {   // 12-hour mode: bit 7 is the PM flag
        pm = 1;
        raw_hour &= 0x7F;
    }
    out->hour = field(raw_hour, binary);
    if (h12) {
        if (out->hour == 12) out->hour = pm ? 12 : 0;   // 12 AM is 00
        else if (pm) out->hour = (unsigned char)(out->hour + 12);
    }

    out->dow   = field(regs[RTC_R_DOW], binary);
    out->day   = field(regs[RTC_R_DAY], binary);
    out->month = field(regs[RTC_R_MON], binary);

    // The century register is optional and some chips leave it at 0, so a
    // value that is not a plausible century falls back to rtc_init()'s.
    unsigned int y = field(regs[RTC_R_YEAR], binary);
    unsigned char cen = regs[RTC_R_CENTURY];
    unsigned int c = century;
    if (!binary && cen >= 0x10 && cen <= 0x99) c = bcd2bin(cen);
    else if (binary && cen >= 10 && cen <= 99) c = cen;
    out->year = c * 100 + y;

    // A calendar time, or a crossed read. This is the check that turns the
    // v38.165 tear into a detectable event rather than a plausible stamp.
    if (out->second > 59 || out->minute > 59 || out->hour > 23) return -1;
    if (out->day < 1 || out->day > 31) return -1;
    if (out->month < 1 || out->month > 12) return -1;
    if (out->dow < 1 || out->dow > 7) return -1;
    if (out->year < 2000 || out->year > 2199) return -1;
    return 0;
}

// Wait for the update cycle to finish. The lock is taken and dropped per
// poll, never held across the wait: the wait can be milliseconds and holding
// a lock with IF=0 that long is how the v38.157 "cli for seconds" class of
// bug starts.
static int rtc_wait_uip_clear(void) {
    for (int tries = 0; tries < RTC_UIP_TRIES; tries++) {
        uint32_t ef = spin_lock_irqsave(&rtc_lock);
        unsigned char reg_a = cmos_read_locked(RTC_REG_A);
        spin_unlock_irqrestore(&rtc_lock, ef);
        if (!(reg_a & RTC_UIP)) return 1;
        for (volatile int d = 0; d < 100; d++);   // ~1us between polls
    }
    return 0;
}

// One locked burst of the whole image. Every index/value pair inside it
// belongs to this caller, and Reg A is read at both ends so the caller can
// tell whether an update began inside the burst.
static void rtc_read_burst(unsigned char* img, unsigned char* reg_a_first,
                           unsigned char* reg_a_last) {
    uint32_t ef = spin_lock_irqsave(&rtc_lock);
    *reg_a_first = cmos_read_locked(RTC_REG_A);
    img[RTC_R_SEC]      = cmos_read_locked(RTC_SEC);
    img[RTC_R_MIN]      = cmos_read_locked(RTC_MIN);
    img[RTC_R_HOUR]     = cmos_read_locked(RTC_HOUR);
    img[RTC_R_DOW]      = cmos_read_locked(RTC_DOW);
    img[RTC_R_DAY]      = cmos_read_locked(RTC_DAY);
    img[RTC_R_MON]      = cmos_read_locked(RTC_MON);
    img[RTC_R_YEAR]     = cmos_read_locked(RTC_YEAR);
    img[RTC_R_CENTURY]  = cmos_read_locked(RTC_CENTURY);
    img[RTC_R_STATUS_B] = cmos_read_locked(RTC_REG_B);
    *reg_a_last  = cmos_read_locked(RTC_REG_A);
    spin_unlock_irqrestore(&rtc_lock, ef);
}

// One line, throttled: this can only fire when an update lands inside every
// retry, so it is a real anomaly and not a per-call event.
static void rtc_note_unstable(int attempts, unsigned char reg_a) {
    static uint32_t last_tick = 0;
    extern volatile uint32_t timer_ticks;
    if (last_tick != 0 && (timer_ticks - last_tick) < 2000) return;
    last_tick = timer_ticks;
    write_serial_string("[RTC] no stable stamp after ");
    write_serial_hex((uint32_t)attempts);
    write_serial_string(" attempts (regA=");
    write_serial_hex(reg_a);
    write_serial_string(")\n");
}

void rtc_init(void) {
    // Probe the century register through the same locked pair every reader
    // uses, so init cannot tear a concurrent SYS_GET_TIME either.
    uint32_t ef = spin_lock_irqsave(&rtc_lock);
    unsigned char c = cmos_read_locked(RTC_CENTURY);
    spin_unlock_irqrestore(&rtc_lock, ef);

    if (c >= 0x10 && c <= 0x99) {
        century = bcd2bin(c);
    } else {
        century = 20; // Assume 2000s
    }
}

rtc_time_t rtc_read_time(void) {
    unsigned char img[RTC_R_COUNT];
    rtc_time_t t;
    unsigned char a0 = 0, a1 = 0;
    int attempt = 0;

    for (attempt = 0; attempt < RTC_READ_ATTEMPTS; attempt++) {
        // Wait for the chip to be out of its update cycle before the burst.
        rtc_wait_uip_clear();

        rtc_read_burst(img, &a0, &a1);

        // Stable: no update started inside the burst, and the image is a
        // real calendar time.
        if (!(a0 & RTC_UIP) && !(a1 & RTC_UIP) &&
            rtc_decode_regs(img, &t) == 0) {
            return t;
        }
    }

    rtc_note_unstable(attempt, a1);

    // Best effort: hand back the decode of the last image, which the caller's
    // own validation (and apps/rtc_stress.c) can see. Nothing is invented, and
    // the line above says it happened.
    (void)rtc_decode_regs(img, &t);
    return t;
}
