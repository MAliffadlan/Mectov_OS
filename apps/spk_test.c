// spk_test.c — SYS_PLAY_SOUND ABI test, from Ring 3.
//
// The syscall was a bare `regs->eax = 0`: it advertised a tone and returned
// success without ever touching the speaker, which is indistinguishable from a
// working beep on a mute card — apps/snake.c, apps/flappy.c and apps/volume.c
// have been calling it for their sound effects all along. Two things follow
// from that, and this app is about both:
//
//   1. the VERDICT. The call validates its arguments and refuses out-of-range
//      ones with -1 (v38.88's rule: a caller must never observe something it
//      did not ask for), so the wrapper has to return that verdict rather than
//      discard it — otherwise "refused" and "played" look identical here.
//      A refactor that quietly reinstates the no-op fails every case below,
//      because a no-op returns 0 for the rejected arguments too.
//
//   2. it is NON-BLOCKING. Every caller is a game loop that fires an effect and
//      carries on; snake's game-over note is 500 ms. A blocking implementation
//      is indistinguishable from a non-blocking one by return code alone, so
//      the last case times a 3 s note against the ms clock instead.
//
// What this app CANNOT see, and does not claim: whether the tone is audible.
// Ring 3 has no access to port 0x61, so "the frequency actually reached the
// PIT" is proved on the host instead, by scripts/spk_deadline_test.py, which
// compiles the shipped driver against a fake PIT and reads back the divisor.
// The floor of 20 Hz is pinned here as the CONTRACT, not as arithmetic: 19 Hz
// would still fit the PIT's 16-bit divisor (1193180/18 = 66287 is the first
// that does not), so 20 is a margin over the overflow point.
#include "src/include/syscall.h"

static int checks = 0;
static int failures = 0;

static void out(const char* s, unsigned char color) { sys_print(s, color); }

static void print_int(int v) {
    char tmp[12];
    char buf[13];
    int n = 0, j = 0;
    unsigned int u = (v < 0) ? (unsigned int)(-v) : (unsigned int)v;

    if (u == 0) tmp[n++] = '0';
    while (u) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
    if (v < 0) tmp[n++] = '-';
    while (n) buf[j++] = tmp[--n];
    buf[j] = '\0';
    out(buf, 0x0B);
}

// One call, one line of evidence: the arguments, the verdict the syscall
// returned, and whether that is the verdict the ABI promises.
static void expect_tone(int freq, int duration_ms, int want) {
    int rc = sys_play_sound(freq, duration_ms);

    out("[SPK] play(freq=", 0x0B);
    print_int(freq);
    out(", ms=", 0x0B);
    print_int(duration_ms);
    out(") -> ", 0x0B);
    print_int(rc);
    out(rc == want ? "  ok\n" : "  WRONG\n", rc == want ? 0x0A : 0x0C);

    checks++;
    if (rc != want) failures++;
}

void _start(void) {
    out("[SPK] SYS_PLAY_SOUND ABI test\n", 0x0B);

    // ---- the contract's edges, accepted ----------------------------------
    expect_tone(440, 100, 0);          // an ordinary note
    expect_tone(20, 0, 0);             // floor frequency, zero-length note
    expect_tone(20000, 5000, 0);       // ceiling frequency, longest note

    // ---- and one step outside, refused -----------------------------------
    expect_tone(19, 100, -1);          // one Hz under the floor
    expect_tone(20001, 100, -1);       // one Hz over the ceiling
    expect_tone(0, 100, -1);           // no pitch at all
    expect_tone(-5, 100, -1);          // negative frequency
    expect_tone(440, -1, -1);          // negative duration
    expect_tone(440, 5001, -1);        // one ms over the longest note

    // ---- non-blocking: a 3 s note must not cost 3 s ----------------------
    // sys_get_ticks() counts milliseconds. The threshold is 500: a blocking
    // note of 3000 ms overshoots it by more than 2.5 s, while the largest
    // legitimate delay here is scheduling jitter on one int 0x80.
    {
        uint32_t t0 = sys_get_ticks();
        int rc = sys_play_sound(440, 3000);
        uint32_t elapsed = sys_get_ticks() - t0;

        out("[SPK] a 3000 ms note returned after ", 0x0B);
        print_int((int)elapsed);
        out(" ms\n", 0x0B);

        checks++;
        if (rc != 0) {
            failures++;
            out("[SPK] WRONG: the 3 s note was refused\n", 0x0C);
        } else if (elapsed >= 500) {
            failures++;
            out("[SPK] WRONG: the call blocked (a game loop would freeze)\n", 0x0C);
        } else {
            out("[SPK] ok: it returned straight away\n", 0x0A);
        }
    }

    // Leave the speaker quiet: a zero-length note is in range and is silence,
    // so nothing is still pending when this task exits. (Without this the 3 s
    // note above would still be sounding after the app is gone — the tick
    // would cut it, but a test that leaves hardware armed is a test that
    // hides the next failure behind its own noise.)
    {
        int rc = sys_play_sound(440, 0);
        checks++;
        if (rc != 0) {
            failures++;
            out("[SPK] WRONG: a zero-length note was refused\n", 0x0C);
        } else {
            out("[SPK] ok: a zero-length note silences the speaker\n", 0x0A);
        }
    }

    out("[SPK] checks=", 0x0B);
    print_int(checks);
    out(" failures=", 0x0B);
    print_int(failures);
    out("\n", 0x0B);

    if (failures) {
        out("[SPK] ALL TESTS FAILED\n", 0x0C);
        sys_exit_with_code(1);
    }
    out("[SPK] ALL TESTS PASSED\n", 0x0A);
    sys_exit_with_code(0);
}
