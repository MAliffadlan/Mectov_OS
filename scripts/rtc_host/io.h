// scripts/rtc_host/io.h — host shim for src/include/io.h (v38.165).
//
// scripts/rtc_decode_test.py compiles the SHIPPED src/drivers/rtc.c for the
// host with only its "#include ../include/..." lines rewritten to these
// shims, so the driver's own port accesses land in a fake CMOS chip instead
// of the hardware. That is what makes the deterministic cases possible at
// all: an update can be made to land inside a burst on demand, and a
// binary-mode or 12-hour RTC can be presented without such a machine.
//
// The chip's behaviour and counters live in rtc_host.c; see the assertions
// in scripts/rtc_host/main.c for what each counter means.
#ifndef RTC_HOST_IO_H
#define RTC_HOST_IO_H

#include "types.h"

extern unsigned char rtc_host_regs[64];       // the CMOS register file
extern unsigned char rtc_host_pending[64];    // image swapped in mid-burst
extern unsigned char rtc_host_index;          // last register selected
extern long rtc_host_data_reads;              // reads of port 0x71
extern long rtc_host_reg_a_reads;             // ... of which selected Reg A
extern long rtc_host_port70_writes;
extern long rtc_host_swap_after;              // -1 = disabled
extern long rtc_host_uip_on_reg_a_read;       // -1 = never

void rtc_host_outb(unsigned short port, unsigned char val);
unsigned char rtc_host_inb(unsigned short port);

static inline void outb(unsigned short port, unsigned char val) {
    rtc_host_outb(port, val);
}

static inline unsigned char inb(unsigned short port) {
    return rtc_host_inb(port);
}

// The driver only touches the byte ports; the wider ones exist so a future
// use still compiles (and is a no-op rather than a host segfault).
static inline unsigned short inw(unsigned short port) { (void)port; return 0; }
static inline void outw(unsigned short port, unsigned short val) { (void)port; (void)val; }
static inline unsigned int inl(unsigned short port) { (void)port; return 0; }
static inline void outl(unsigned short port, unsigned int val) { (void)port; (void)val; }

#endif
