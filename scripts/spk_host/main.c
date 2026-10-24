// scripts/spk_host/main.c — deterministic tone-deadline cases for speaker.c.
//
// Compiled together with the SHIPPED src/drivers/speaker.c and a fake PIT, and
// driven by scripts/spk_deadline_test.py. Every case goes through the driver's
// two public entry points (speaker_tone / speaker_tick) with the clock moved by
// hand, which is the only way to test a deadline without waiting for one:
//
//   reset        the gate starts closed and no divisor is programmed
//   pitch        tone(440,100)    the PIT divisor really is 1193180/440
//   not-early    tick before it   the note is not cut short
//   deadline     tick at it       the note ends exactly there
//   idle         tick after it    a cleared tone writes nothing more
//   pitch 2      tone(800,50)     a second pitch reaches the PIT too, so the
//                                 divisor follows the argument and not a
//                                 constant that happens to match case 1
//   zero/neg     duration <= 0    silence now, nothing left pending
//   no pitch     freq <= 0        nothing is programmed into the PIT
//   wrap A       the deadline     arm at 0xFFFFFF00 + 500 ms: the deadline
//                wraps            wraps to 244, and the note has to survive
//                                 the wrap and end at 244
//   wrap B       the counter      arm at 0xFFFFFFF0 + 12 ms so the deadline
//                wraps            falls just short of 0xFFFFFFFF and the next
//                                 tick lands past 0
//
// The two wrap cases fail in OPPOSITE directions on a bare unsigned
// `get_ticks() >= tone_off_ms`: wrap A cuts the note one tick after it starts,
// while wrap B never ends it. Both forms are covered, since fixing one
// direction alone could hide the other. The replacement case proves that a
// timer IRQ cannot retire a new pitch using the old note's deadline.
//
// The kernel headers come FIRST on purpose: src/include/types.h defines NULL
// unconditionally, and a libc header included before it would trigger a
// "NULL redefined" warning — this harness builds with the project's
// zero-warning rule.
#include "speaker.h"
#include "io.h"
#include "spinlock.h"
#include "timer.h"

#include <stdio.h>

static int checks = 0;
static int failures = 0;

static void expect(int cond, const char* what) {
    checks++;
    if (cond) {
        printf("[OK] %s\n", what);
    } else {
        failures++;
        printf("[FAIL] %s\n", what);
    }
}

// Bits 0 and 1 of port 0x61 are the timer-2 gate and the speaker data enable;
// play_sound() sets both through one read-modify-write, nosound() clears them.
// "The speaker is making its tone" is exactly those two bits being set.
static int gate_open(void) { return (spk_host_port61 & 3) == 3; }

static void at(uint32_t ms) { spk_host_ticks = ms; }

// Interrupt after the new pitch has been programmed but before get_ticks()
// publishes its new deadline. The old note's deadline has passed, so a tick is
// entitled to retire that old note. It must not retire the new pitch instead.
// The fake clock models local IRQ masking: with an IRQ-safe task transaction
// this tick runs on irqrestore, once the whole replacement is visible.
static int race_hook_fired = 0;

static void tick_during_pitch_programming(void) {
    race_hook_fired++;
    spk_host_before_tick_read = 0;     // do not recurse into this probe
    if (spk_host_irqs_enabled) {
        spk_host_fire_timer_irq();
    } else {
        spk_host_timer_irq_pending = 1;
    }
}

int main(void) {
    long writes;

    // ---- reset: nothing programmed, nothing sounding ----------------------
    spk_host_reset();
    expect(!gate_open(), "reset: the speaker gate starts closed");
    expect(spk_host_divisor_writes == 0, "reset: no divisor programmed yet");

    // ---- a tone programs the PIT and opens the gate -----------------------
    at(1000);
    speaker_tone(440, 100);
    expect(gate_open(), "440 Hz: the gate is opened (there is a tone to hear)");
    expect(spk_host_divisor == 2711u,
           "440 Hz: the PIT divisor is 1193180/440 = 2711, so the frequency "
           "reached the hardware");

    // ---- the deadline, from both sides -----------------------------------
    at(1099);                       // one millisecond early
    speaker_tick();
    expect(gate_open(), "before the deadline: the note is not cut short");

    at(1100);                       // the deadline itself
    speaker_tick();
    expect(!gate_open(), "at the deadline: the note ends");

    writes = spk_host_port61_writes;
    at(1200); at(1400);
    speaker_tick();
    speaker_tick();
    expect(spk_host_port61_writes == writes,
           "idle: once the tone is over, a tick writes nothing at all");

    // ---- a second pitch: the divisor follows the argument -----------------
    at(2000);
    speaker_tone(800, 50);
    expect(spk_host_divisor == 1491u,
           "800 Hz: the divisor is 1193180/800 = 1491, not a constant");
    expect(gate_open(), "800 Hz: the gate is opened again");
    at(2050);
    speaker_tick();
    expect(!gate_open(), "800 Hz: that note ends at its own deadline");

    // ---- a note with no length is silence, not a one-tick note ------------
    // The count is taken AFTER the call: opening and closing the gate is what
    // a zero-length note legitimately does, so the question is only whether
    // anything is still pending once it returns.
    at(3000);
    speaker_tone(440, 0);
    expect(!gate_open(), "zero duration: the tone is silenced immediately");
    writes = spk_host_port61_writes;
    at(3100);
    speaker_tick();
    expect(spk_host_port61_writes == writes && !gate_open(),
           "zero duration: nothing was left pending for a later tick");

    at(3500);
    speaker_tone(440, -5);
    expect(!gate_open(), "negative duration: silenced immediately too");

    // ---- freq <= 0 must never reach the PIT ------------------------------
    // play_sound() divides by the frequency, so the guard that keeps a bad
    // value out of the divisor is worth pinning even though the syscall
    // refuses these arguments before they get here.
    writes = spk_host_divisor_writes;
    at(4000);
    speaker_tone(0, 100);
    expect(spk_host_divisor_writes == writes,
           "freq <= 0: nothing is programmed into the PIT");

    // ---- wrap A: the deadline wraps past 2^32 ----------------------------
    // The millisecond clock is advanced by TIMER_MS_PER_TICK in the kernel, so
// 2^32 ms is ~49.7 days of uptime.
    // Armed at 0xFFFFFF00 for 500 ms, the deadline is 244: numerically far
    // BELOW the counter, but 500 ms ahead of it.
    spk_host_reset();
    at(0xFFFFFF00u);
    speaker_tone(440, 500);
    expect(gate_open(), "wrap A: the note starts");

    speaker_tick();                 // counter unmoved, deadline already wrapped
    expect(gate_open(),
           "wrap A: a deadline that wrapped is not mistaken for one in the past");

    at(0xFFFFFF50u);
    speaker_tick();
    expect(gate_open(), "wrap A: still sounding in the middle of the note");

    at(243u);
    speaker_tick();
    expect(gate_open(), "wrap A: not silenced a millisecond early");

    at(244u);                       // 0xFFFFFF00 + 500 ms, mod 2^32
    speaker_tick();
    expect(!gate_open(), "wrap A: it ends at the wrapped deadline");

    // ---- wrap B: the counter wraps, the deadline does not ----------------
    // Armed at 0xFFFFFFF0 for 12 ms the deadline is 0xFFFFFFFC — still ahead
    // of the counter, but the next tick after 0xFFFFFFFA lands past 0.
    spk_host_reset();
    at(0xFFFFFFF0u);
    speaker_tone(440, 12);

    at(0xFFFFFFFAu);
    speaker_tick();
    expect(gate_open(), "wrap B: not silenced before its deadline");

    at(4u);                         // the same deadline, seen after the wrap
    speaker_tick();
    expect(!gate_open(),
           "wrap B: it still ends, instead of waiting ~49 days for the "
           "counter to come back around");

    // ---- IRQ during a replacement tone's PIT transaction -----------------
    // This is the exact task/IRQ interleaving that publish-order alone cannot
    // protect: the old deadline has expired, the new divisor has reached the
    // PIT, and the new deadline is not published yet. A correct implementation
    // makes the tick either retire the old note before the new pitch is
    // programmed or see the whole new transaction; it must not kill the new
    // note in between.
    spk_host_reset();
    at(1000);
    speaker_tone(440, 100);          // old deadline: 1100
    at(1100);
    spk_host_before_tick_read = tick_during_pitch_programming;
    speaker_tone(880, 1000);         // tick lands before the new clock read
    spk_host_before_tick_read = 0;
    printf("       replacement state: gate=0x%02x divisor=%u ticks=%u\n",
           spk_host_port61, spk_host_divisor, (unsigned)spk_host_ticks);
    expect(race_hook_fired == 1,
           "tick during pitch programming: the IRQ probe actually fired");
    expect(gate_open(),
           "tick during pitch programming: the replacement gate is open");
    expect(spk_host_divisor == 1355u,
           "tick during pitch programming: 880 Hz divisor is 1193180/880 = 1355");
    at(1200);
    speaker_tone(660, 0);            // no stale new deadline may survive
    expect(!gate_open(), "tick during pitch programming: no stale pending tone");
    at(1300);
    speaker_tone(660, 50);
    at(1350);
    speaker_tick();
    expect(!gate_open(), "the next tone still ends at its own deadline");

    // ---- a blocking legacy note cancels the old non-blocking owner -------
    // nada()/beep() use the same PIT and gate. Once they finish, their old
    // deadline must not still be armed: a later tick must not write the port.
    spk_host_reset();
    at(2000);
    speaker_tone(440, 500);          // old deadline: 2500
    nada(880, 1);
    writes = spk_host_port61_writes;
    at(2500);
    speaker_tick();
    expect(spk_host_port61_writes == writes,
           "legacy nada: the old non-blocking deadline is cancelled");

    spk_host_reset();
    at(3000);
    speaker_tone(440, 500);          // old deadline: 3500
    beep();
    writes = spk_host_port61_writes;
    at(3500);
    speaker_tick();
    expect(spk_host_port61_writes == writes,
           "legacy beep: the old non-blocking deadline is cancelled");

    // ---- and the driver pokes nothing else -------------------------------
    expect(spk_host_other_writes == 0 && spk_host_other_reads == 0,
           "the driver touches only ports 0x42, 0x43 and 0x61");

    printf("spk deadline checks=%d failures=%d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
