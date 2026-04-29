// ============================================================
// rtc.c — Mectov OS CMOS Real-Time Clock Driver
// ============================================================
// Reads time/date from the MC146818 RTC chip (CMOS).
// Uses proper Update-In-Progress (UIP) check and NMI-safe access.
// ============================================================

#include "../include/rtc.h"
#include "../include/io.h"

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

// NMI disable bit (bit 7 of address port)
#define NMI_DISABLE 0x80

static unsigned char century = 20; // Default: 2000s

// Read a CMOS register while keeping NMI disabled
static inline unsigned char cmos_read(unsigned char reg) {
    outb(CMOS_ADDR, NMI_DISABLE | reg);
    return inb(CMOS_DATA);
}

// Convert BCD to binary
static inline unsigned char bcd2bin(unsigned char bcd) {
    return ((bcd & 0xF0) >> 1) + ((bcd & 0xF0) >> 3) + (bcd & 0x0F);
}

void rtc_init(void) {
    // Try to read century register
    unsigned char c = cmos_read(RTC_CENTURY);
    if (c >= 0x10 && c <= 0x99) {
        century = bcd2bin(c);
    } else {
        century = 20; // Assume 2000s
    }
}

rtc_time_t rtc_read_time(void) {
    rtc_time_t t;
    unsigned char reg_a;
    int tries = 0;

    // Wait for UIP to clear (RTC not updating)
    // UIP goes high 244us before each update; wait max 10ms
    do {
        reg_a = cmos_read(RTC_REG_A);
        if (!(reg_a & RTC_UIP)) break;
        // Small busy-wait delay (~1us per iteration)
        for (volatile int d = 0; d < 100; d++);
        tries++;
    } while (tries < 10000);

    // Read all registers atomically
    unsigned char sec   = cmos_read(RTC_SEC);
    unsigned char min   = cmos_read(RTC_MIN);
    unsigned char hour  = cmos_read(RTC_HOUR);
    unsigned char dow   = cmos_read(RTC_DOW);
    unsigned char day   = cmos_read(RTC_DAY);
    unsigned char mon   = cmos_read(RTC_MON);
    unsigned char yr    = cmos_read(RTC_YEAR);
    unsigned char cen   = cmos_read(RTC_CENTURY);

    // Convert BCD to binary
    t.second = bcd2bin(sec);
    t.minute = bcd2bin(min);
    t.hour   = bcd2bin(hour);
    t.dow    = bcd2bin(dow);
    t.day    = bcd2bin(day);
    t.month  = bcd2bin(mon);

    // Determine full year
    unsigned int y = bcd2bin(yr);
    unsigned int c;
    if (cen >= 0x10 && cen <= 0x99) {
        c = bcd2bin(cen);
    } else {
        c = century;
    }
    t.year = c * 100 + y;

    return t;
}