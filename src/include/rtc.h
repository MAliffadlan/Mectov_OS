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

// Initialize RTC
void rtc_init(void);

// Read current time from CMOS RTC with UIP (Update-In-Progress) check
rtc_time_t rtc_read_time(void);

#endif