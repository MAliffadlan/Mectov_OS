// rtc_stress — v38.165 concurrent RTC read-consistency test (audit: RTC
// locked/torn).
//
// A CMOS register read is a two-port transaction: write the register index
// to 0x70, then read the value from 0x71. Nothing serialized that pair, so
// with several CPUs reading the clock at once — SYS_GET_TIME from any task,
// the taskbar/shell clock, the timer's per-second rate calibration — one
// CPU's inb(0x71) can return the register ANOTHER CPU selected in between.
// The result is a time assembled from mixed fields (torn): e.g. a "second"
// that is really the minute register's value, or a year/count swapped in
// from a neighbouring register.
//
// The detector uses the fact that two reads a few microseconds apart must
// observe the SAME wall-clock second (the RTC updates once per second):
//
//   * every stamp is range-checked (a crossed read decodes to out-of-range
//     garbage often enough to matter, and never for a healthy read);
//   * two back-to-back stamps must be within one second of each other — the
//     only legitimate difference is the second boundary falling between
//     them, and a third read breaks that tie;
//   * 4 reader threads hammering sys_get_time() on 4 cores make the port
//     pairs interleave constantly. A healthy driver (one lock around the
//     addr/data pair) produces ZERO torn stamps in tens of thousands.
//
// Three outcomes are counted SEPARATELY, because they are different bugs:
//
//   checked    — a stamp pair that agreed (the only good outcome)
//   torn       — both calls succeeded, but the stamp is impossible: the
//                driver handed back registers from different instants
//   refused    — SYS_GET_TIME returned non-zero (the kernel declined)
//   unwritten  — the call reported success but the buffer still holds the
//                pre-fill sentinel, i.e. nothing was written at all
//
// A sentinel pre-fill is what makes `unwritten` detectable: a stamp that was
// never written is otherwise indistinguishable from one that happens to
// decode to zeros. The verdict fails on ANY of torn/refused/unwritten > 0.
//
// Verdicts printed to the terminal (also to serial via sys_print):
//      [RTC_STRESS] PASS checked=<n> refused=0 unwritten=0
//      [RTC_STRESS] FAIL torn=<n> of <m> refused=<r> unwritten=<u>
//      [RTC_STRESS] example id=<i> a=<h:m:s d/m/yr> b=<h:m:s d/m/yr>
//      run /apps/rtc_stress.mct
//
// scripts/rtc_test.py drives this from the host and asserts the guest's own
// numbers; scripts/rtc_decode_test.py covers the decode/straddle rules
// deterministically on the host (no hardware needed).
#include "src/include/syscall.h"
#include "lib/thread.h"

#define READERS 4
#define ITERS   12000           // per reader; ~tens of thousands of samples
#define SENTINEL 0xAA           // marks a buffer no successful call wrote

static int g_checked[READERS];
static int g_torn[READERS];
static int g_refused[READERS];
static int g_unwritten[READERS];
static rtc_time_t g_ex_a[READERS];   // first torn example per reader
static rtc_time_t g_ex_b[READERS];

static void print_int(int v) {
    char buf[12];
    int i = 0;
    if (v == 0) { sys_print("0", 0x0B); return; }
    while (v > 0 && i < 11) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) { char c[2] = { buf[--i], '\0' }; sys_print(c, 0x0B); }
}

static void fill_sentinel(rtc_time_t* t) {
    unsigned char* p = (unsigned char*)t;
    for (unsigned int i = 0; i < sizeof(rtc_time_t); i++) p[i] = SENTINEL;
}

static int still_sentinel(const rtc_time_t* t) {
    const unsigned char* p = (const unsigned char*)t;
    for (unsigned int i = 0; i < sizeof(rtc_time_t); i++)
        if (p[i] != SENTINEL) return 0;
    return 1;
}

// Every field a healthy rtc_read_time() can produce. A stamp failing this is
// either a crossed read that decoded to garbage or the RTC is wedged.
static int valid(const rtc_time_t* t) {
    if (t->second > 59) return 0;
    if (t->minute > 59) return 0;
    if (t->hour > 23) return 0;
    if (t->day < 1 || t->day > 31) return 0;
    if (t->month < 1 || t->month > 12) return 0;
    if (t->dow < 1 || t->dow > 7) return 0;
    if (t->year < 2020 || t->year > 2099) return 0;
    return 1;
}

// Seconds since 2000-01-01 00:00:00 for a validated stamp (only the hour/
// minute/second part ever moves during a run; the full date keeps the
// arithmetic honest across a midnight rollover).
static long day_number(const rtc_time_t* t) {
    static const unsigned char mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    long days = 0;
    for (unsigned int y = 2000; y < t->year && y < 2100; y++) {
        int leap = (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
        days += leap ? 366 : 365;
    }
    int leap = (t->year % 4 == 0) && (t->year % 100 != 0 || t->year % 400 == 0);
    for (unsigned int m = 1; m < t->month && m <= 12; m++)
        days += mdays[m - 1] + ((m == 2 && leap) ? 1 : 0);
    days += (long)t->day - 1;
    return days;
}

static long epoch_secs(const rtc_time_t* t) {
    return ((day_number(t) * 24 + t->hour) * 60 + t->minute) * 60 + t->second;
}

static long distance(long a, long b) { return a > b ? a - b : b - a; }

static void reader(void* arg) {
    int id = (int)(unsigned int)arg;
    int checked = 0, torn = 0, refused = 0, unwritten = 0;

    for (int i = 0; i < ITERS; i++) {
        rtc_time_t a, b, c;
        fill_sentinel(&a);
        fill_sentinel(&b);
        int rca = syscall(SYS_GET_TIME, (int)(unsigned int)&a, 0, 0);
        int rcb = syscall(SYS_GET_TIME, (int)(unsigned int)&b, 0, 0);

        if (rca != 0 || rcb != 0) { refused++; continue; }
        if (still_sentinel(&a) || still_sentinel(&b)) { unwritten++; continue; }

        long da = valid(&a) ? epoch_secs(&a) : -1;
        long db = valid(&b) ? epoch_secs(&b) : -1;
        if (da >= 0 && db >= 0 && distance(da, db) <= 1) {
            checked++;
            continue;
        }
        // Mismatch: a third read breaks the tie (the second boundary may
        // have fallen between the first two).
        fill_sentinel(&c);
        int rcc = syscall(SYS_GET_TIME, (int)(unsigned int)&c, 0, 0);
        if (rcc != 0) { refused++; continue; }
        if (valid(&c)) {
            long dc = epoch_secs(&c);
            if ((da >= 0 && distance(da, dc) <= 1) ||
                (db >= 0 && distance(db, dc) <= 1)) {
                checked++;
                continue;
            }
        }
        if (torn == 0) { g_ex_a[id] = a; g_ex_b[id] = b; }
        torn++;
    }

    g_checked[id] = checked;
    g_torn[id] = torn;
    g_refused[id] = refused;
    g_unwritten[id] = unwritten;
    sys_print("[RTC_STRESS] reader done id=", 0x0B);
    print_int(id);
    sys_print(" checked=", 0x0B);
    print_int(checked);
    sys_print(" torn=", 0x0B);
    print_int(torn);
    sys_print(" refused=", 0x0B);
    print_int(refused);
    sys_print(" unwritten=", 0x0B);
    print_int(unwritten);
    sys_print("\n", 0x0B);
}

static void dump_stamp(const rtc_time_t* t) {
    print_int(t->hour);   sys_print(":", 0x0E);
    print_int(t->minute); sys_print(":", 0x0E);
    print_int(t->second); sys_print(" ", 0x0E);
    print_int(t->day);    sys_print("/", 0x0E);
    print_int(t->month);  sys_print("/", 0x0E);
    print_int((int)t->year);
}

void _start(void) {
    sys_print("[RTC_STRESS] start readers=", 0x0E);
    print_int(READERS);
    sys_print(" iters=", 0x0E);
    print_int(ITERS);
    sys_print("\n", 0x0E);

    if (mct_tls_init() != 0) {
        sys_print("[RTC_STRESS] FAIL tls-init\n", 0x0C);
        sys_exit_with_code(1);
    }

    int tids[READERS];
    for (int i = 0; i < READERS; i++) {
        tids[i] = mct_thread_create(reader, (void*)(unsigned int)i);
        if (tids[i] < 0) {
            sys_print("[RTC_STRESS] FAIL create reader\n", 0x0C);
            sys_exit_with_code(1);
        }
    }
    for (int i = 0; i < READERS; i++) {
        if (mct_thread_join(tids[i]) != tids[i]) {
            sys_print("[RTC_STRESS] FAIL join reader\n", 0x0C);
            sys_exit_with_code(1);
        }
    }

    int checked = 0, torn = 0, refused = 0, unwritten = 0;
    for (int i = 0; i < READERS; i++) {
        checked += g_checked[i];
        torn += g_torn[i];
        refused += g_refused[i];
        unwritten += g_unwritten[i];
    }

    if (torn == 0 && refused == 0 && unwritten == 0) {
        sys_print("[RTC_STRESS] PASS checked=", 0x0A);
        print_int(checked);
        sys_print(" refused=0 unwritten=0\n", 0x0A);
        sys_exit_with_code(0);
    }

    // Failure: totals plus one example per reader that saw a tear, for
    // post-mortem. refused/unwritten are reported separately because they are
    // different defects (a declined call / an untouched buffer) and must not
    // be read as a crossed read.
    sys_print("[RTC_STRESS] FAIL torn=", 0x0C);
    print_int(torn);
    sys_print(" of ", 0x0C);
    print_int(torn + checked);
    sys_print(" refused=", 0x0C);
    print_int(refused);
    sys_print(" unwritten=", 0x0C);
    print_int(unwritten);
    sys_print("\n", 0x0C);
    for (int i = 0; i < READERS; i++) {
        if (g_torn[i] == 0) continue;
        sys_print("[RTC_STRESS] example id=", 0x0E);
        print_int(i);
        sys_print(" a=", 0x0E);
        dump_stamp(&g_ex_a[i]);
        sys_print(" b=", 0x0E);
        dump_stamp(&g_ex_b[i]);
        sys_print("\n", 0x0E);
    }
    sys_exit_with_code(1);
}
