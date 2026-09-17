/* M4 demo 2: FPU isolation — SSE double + x87 long double accumulators
 * across voluntary yields AND timer preemptions. Any FPU-state leak between
 * tasks changes the exact expected values below.
 */
#include "sys64.h"

/* Fixed 6-fraction line emitter (exact-value checks, no float printf). */
static void emit_double(dline_t *l, double d) {
    if (d < 0) { dl_s(l, "-"); d = -d; }
    unsigned long long ip = (unsigned long long)d;
    dl_u(l, ip);
    dl_s(l, ".");
    unsigned long long frac = (unsigned long long)((d - (double)ip) * 1000000.0);
    unsigned long long div = 100000;
    for (int i = 0; i < 6; i++) {
        char c = (char)('0' + (frac / div) % 10);
        char tmp[2] = { c, '\0' };
        dl_s(l, tmp);
        div /= 10;
    }
}

void demo_main(void) {
    DLINE(l);
    long pid = d_pid();
    double dsum = 0.0; /* SSE2: cvtsi2sd/addsd */
    for (int i = 1; i <= 1000; i++) {
        dsum += (double)i;
        if ((i % 100) == 0) d_yield();
    }
    float fsum = 0.0f; /* SSE: addss */
    for (int i = 0; i < 2000; i++) {
        fsum += 0.5f;
        if ((i % 200) == 0) d_yield();
    }
    long double ld = 0.0L; /* x87: fld/fadd/fstp */
    for (int i = 0; i < 1000; i++) {
        ld += 1.0L;
        if ((i % 100) == 0) d_yield();
    }
    dl_s(&l, "FPU d=");
    emit_double(&l, dsum);
    dl_s(&l, " f=");
    emit_double(&l, (double)fsum);
    dl_s(&l, " ld=");
    dl_u(&l, (u64)ld);
    dl_s(&l, " pid=");
    dl_u(&l, (u64)pid);
    dl_nl(&l);
    dl_s(&l, "FPU-DONE");
    dl_nl(&l);
}
