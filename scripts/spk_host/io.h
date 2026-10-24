// scripts/spk_host/io.h — host shim for src/include/io.h (tone deadline gate).
//
// scripts/spk_deadline_test.py compiles the SHIPPED src/drivers/speaker.c for
// the host with only its "#include ../include/..." lines rewritten to these
// shims, so the driver's own port accesses land in a fake PIT + speaker gate
// instead of the hardware. The real inb/outb are privileged instructions: on
// the host they fault long before an assertion could run, so the port traffic
// has to be captured here for the driver's pitch to be checkable at all.
//
// The port model and the clock live in spk_host.c; see the assertions in
// scripts/spk_host/main.c for what each counter means.
#ifndef SPK_HOST_IO_H
#define SPK_HOST_IO_H

#include "types.h"

extern unsigned char spk_host_port61;        // the speaker gate register
extern unsigned char spk_host_pit_cmd;       // last command byte sent to 0x43
extern unsigned int  spk_host_divisor;       // last complete 16-bit divisor (0x42)
extern long spk_host_divisor_writes;         // completed divisor writes
extern long spk_host_port61_writes;          // writes to the gate register
extern long spk_host_other_writes;           // writes to any other port
extern long spk_host_other_reads;            // reads of any other port

// The clock and its optional IRQ probe are provided by
// scripts/spk_host/timer.h and spk_host.c. The probe runs at the get_ticks()
// boundary, after the pitch is programmed and before its deadline is published;
// no port hook or production-driver instrumentation is needed.

void spk_host_reset(void);
void spk_host_outb(unsigned short port, unsigned char val);
unsigned char spk_host_inb(unsigned short port);

static inline void outb(unsigned short port, unsigned char val) {
    spk_host_outb(port, val);
}

static inline unsigned char inb(unsigned short port) {
    return spk_host_inb(port);
}

// The driver only touches the byte ports; the wider ones exist so a future
// use still compiles (and is a no-op rather than a host segfault).
static inline unsigned short inw(unsigned short port) { (void)port; return 0; }
static inline void outw(unsigned short port, unsigned short val) { (void)port; (void)val; }
static inline unsigned int inl(unsigned short port) { (void)port; return 0; }
static inline void outl(unsigned short port, unsigned int val) { (void)port; (void)val; }

#endif
