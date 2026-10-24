// scripts/spk_host/spk_host.c — the fake PIT + speaker gate the driver pokes.
//
// play_sound() is three port writes and a read-modify-write of the gate:
//   0x43 <- 0xb6            PIT channel 2, mode 3, lo/hi byte
//   0x42 <- divisor & 0xff  the tone's frequency, as a 16-bit countdown
//   0x42 <- divisor >> 8
//   0x61 <- inb(0x61) | 3   timer 2 gate + speaker data enable
// nosound() clears the low two bits of 0x61. Modelling exactly that is enough
// to answer the question that matters for a non-blocking tone: did the caller's
// frequency actually reach the PIT, and is the gate still open when the tick
// says the note should be over?
#include "io.h"
#include "spinlock.h"
#include "speaker.h"
#include "timer.h"

unsigned char spk_host_port61 = 0;
unsigned char spk_host_pit_cmd = 0;
unsigned int  spk_host_divisor = 0;
long spk_host_divisor_writes = 0;
long spk_host_port61_writes = 0;
long spk_host_other_writes = 0;
long spk_host_other_reads = 0;
void (*spk_host_before_tick_read)(void) = 0;
volatile uint32_t spk_host_ticks = 0;
int spk_host_irqs_enabled = 1;
int spk_host_timer_irq_pending = 0;

uint32_t get_ticks(void) {
    if (spk_host_before_tick_read) spk_host_before_tick_read();
    return spk_host_ticks;
}

void spk_host_fire_timer_irq(void) {
    speaker_tick();
}

void spk_host_irq_restore(uint32_t eflags) {
    spk_host_irqs_enabled = (eflags & 0x200u) != 0;
    if (spk_host_irqs_enabled && spk_host_timer_irq_pending) {
        spk_host_timer_irq_pending = 0;
        spk_host_fire_timer_irq();
    }
}

// The divisor is written as two bytes; both have to arrive before the PIT has
// a frequency, so the low byte alone is not a write.
static int pit_lo_pending = -1;

void spk_host_reset(void) {
    spk_host_port61 = 0;
    spk_host_pit_cmd = 0;
    spk_host_divisor = 0;
    spk_host_divisor_writes = 0;
    spk_host_port61_writes = 0;
    spk_host_other_writes = 0;
    spk_host_other_reads = 0;
    spk_host_ticks = 0;
    spk_host_irqs_enabled = 1;
    spk_host_timer_irq_pending = 0;
    spk_host_before_tick_read = 0;
    pit_lo_pending = -1;
}

void spk_host_outb(unsigned short port, unsigned char val) {
    if (port == 0x43) {
        spk_host_pit_cmd = val;
        return;
    }
    if (port == 0x42) {
        if (pit_lo_pending < 0) {
            pit_lo_pending = val;
            return;
        }
        spk_host_divisor = (unsigned int)((unsigned)pit_lo_pending | ((unsigned)val << 8));
        pit_lo_pending = -1;
        spk_host_divisor_writes++;
        return;
    }
    if (port == 0x61) {
        spk_host_port61 = val;
        spk_host_port61_writes++;
        return;
    }
    spk_host_other_writes++;
}

unsigned char spk_host_inb(unsigned short port) {
    // 0x61 is the only port the driver reads; anything else counts as a
    // surprise and is asserted against in main.c.
    if (port == 0x61) {
        return spk_host_port61;
    }
    spk_host_other_reads++;
    return 0xFF;
}
