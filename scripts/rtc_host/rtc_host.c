// scripts/rtc_host/rtc_host.c — the fake CMOS chip behind scripts/rtc_host/io.h.
//
// Models the two-port transaction exactly as the chip does: port 0x70 selects
// a register, port 0x71 returns whatever is selected AT THAT MOMENT. A script
// can therefore do the two things a real chip only does occasionally:
//
//   * swap the image in the middle of a burst (rtc_host_swap_after), i.e. the
//     update landing between two register reads — the straddle;
//   * report UIP on a chosen Reg A read (rtc_host_uip_on_reg_a_read), which is
//     how the driver is told the burst it just finished is not trustworthy.
//
// Both are single-threaded here on purpose: this harness tests the driver's
// DECISION LOGIC. Whether two CPUs can interleave the pair is not a question a
// single-threaded shim can answer, and that half is measured in the guest.
// Kernel headers first — src/include/types.h defines NULL unconditionally,
// so a libc header included before it warns (see main.c).
#include "io.h"
#include "serial.h"

#include <string.h>

unsigned char rtc_host_regs[64];
unsigned char rtc_host_pending[64];
unsigned char rtc_host_index = 0;
long rtc_host_data_reads = 0;
long rtc_host_reg_a_reads = 0;
long rtc_host_port70_writes = 0;
long rtc_host_swap_after = -1;
long rtc_host_uip_on_reg_a_read = -1;

char rtc_host_serial[4096];
unsigned long rtc_host_serial_len = 0;

// The driver's throttled anomaly line reads the kernel's tick counter.
volatile uint32_t timer_ticks = 0;

static void rtc_host_serial_put(const char* s) {
    while (*s && rtc_host_serial_len < sizeof(rtc_host_serial) - 1)
        rtc_host_serial[rtc_host_serial_len++] = *s++;
    rtc_host_serial[rtc_host_serial_len] = '\0';
}

void write_serial(char a) {
    char b[2] = { a, '\0' };
    rtc_host_serial_put(b);
}

void write_serial_string(const char* str) {
    rtc_host_serial_put(str ? str : "");
}

void write_serial_hex(uint32_t val) {
    char buf[4] = { '0', 'x', 0, 0 };
    static const char hexd[] = "0123456789ABCDEF";
    char out[11];
    int i;
    out[0] = '0'; out[1] = 'x';
    for (i = 0; i < 8; i++)
        out[2 + i] = hexd[(val >> (28 - 4 * i)) & 0xF];
    out[10] = '\0';
    (void)buf;
    rtc_host_serial_put(out);
}

void rtc_host_outb(unsigned short port, unsigned char val) {
    if (port == 0x70) {
        // Bit 7 is the NMI mask; the register index is the low bits. The
        // driver sets it, and the chip must not treat it as part of the index
        // or every read would land on an unimplemented register.
        rtc_host_index = (unsigned char)(val & 0x7F);
        rtc_host_port70_writes++;
    } else if (port == 0x71) {
        rtc_host_regs[rtc_host_index] = val;   // not used by the driver
    }
}

unsigned char rtc_host_inb(unsigned short port) {
    if (port != 0x71) return 0xFF;             // nothing else is modelled

    unsigned char idx = rtc_host_index;
    unsigned char v = rtc_host_regs[idx];
    rtc_host_data_reads++;

    if (idx == 0x0A) {                         // Status Register A
        rtc_host_reg_a_reads++;
        if (rtc_host_reg_a_reads == rtc_host_uip_on_reg_a_read) v |= 0x80;
    }

    // The update lands between two register reads: everything read from here
    // on comes from the new image. Applied AFTER returning this read's value,
    // which is what makes a straddle possible at all.
    if (rtc_host_swap_after >= 0 && rtc_host_data_reads >= rtc_host_swap_after) {
        memcpy(rtc_host_regs, rtc_host_pending, sizeof(rtc_host_regs));
        rtc_host_swap_after = -1;
    }
    return v;
}
