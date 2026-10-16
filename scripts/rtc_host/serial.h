// scripts/rtc_host/serial.h — host shim for src/include/serial.h (v38.165).
//
// The driver logs one throttled line when no burst came out stable. Capturing
// it (instead of dropping it) is what lets the straddle case assert the
// POSITIVE shape: the driver recovered by RETRYING and did not have to fall
// back to the best-effort decode.
#ifndef RTC_HOST_SERIAL_H
#define RTC_HOST_SERIAL_H

#include "types.h"

extern char rtc_host_serial[4096];
extern unsigned long rtc_host_serial_len;

void write_serial(char a);
void write_serial_string(const char* str);
void write_serial_hex(uint32_t val);

#endif
