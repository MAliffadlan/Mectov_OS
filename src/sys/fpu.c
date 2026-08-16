// ============================================================
// FPU/SSE context switching (v38.41)
// ============================================================
// Before v38.41 the scheduler never touched the FPU: the kernel core is
// soft-float and DOOM was treated as the system's ONLY FPU user (it runs
// inside the shell's task in kernel mode), so its x87 state survived
// preemption purely because nothing else emitted FPU instructions. Any
// second FPU user — a Ring 3 app built with hard float, another kernel
// module — would have silently corrupted that state.
//
// Now every task carries a 512-byte fxsave image and schedule() swaps it
// eagerly on every context switch (fxsave out, fxrstor in) with IF=0
// under task_lock. "Eager" (as opposed to lazy CR0.TS + #NM schemes)
// costs one fxsave+fxrstor per switch but needs no per-CPU owner
// tracking, which keeps SMP correctness trivial.

#include "../include/fpu.h"
#include "../include/serial.h"
#include "../include/utils.h"   // memcpy

// Set identically on every core by fpu_init_cpu. 0 = this CPU has no
// x87/SSE (checked via CPUID): swap helpers become no-ops and CR0/CR4
// are left exactly as the firmware left them.
static int fpu_enabled = 0;

// Canonical clean image: fninit'd x87 (CW=0x037F, every exception masked)
// and default MXCSR (every SIMD exception masked). memcpy'd into every
// fresh task; a freshly restored image can never raise #XM.
static uint8_t fpu_clean_state[512] __attribute__((aligned(16)));

// CPUID leaf 1 EDX feature flags. CPUID itself is assumed present: GRUB
// (and every bootloader since the 90s) requires it, and so does QEMU/TCG.
static uint32_t fpu_cpuid_edx(void) {
    uint32_t a, b, c, d;
    __asm__ __volatile__(
        "cpuid"
        : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
        : "a"(1)
    );
    return d;
}

void fpu_init_cpu(void) {
    uint32_t edx = fpu_cpuid_edx();
    if (!(edx & (1u << 0)) || !(edx & (1u << 25))) {   // no x87 or no SSE
        fpu_enabled = 0;
        write_serial_string("[FPU] no x87/SSE via CPUID: fxsave switching disabled\n");
        return;
    }

    // CR0: EM=0 (real FPU, no emulation), MP=1, TS=0 (the eager scheme
    // never task-switches the FPU lazily, so #NM on FPU use cannot fire).
    uint32_t cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1u << 2);            // EM
    cr0 |=  (1u << 1);            // MP
    cr0 &= ~(1u << 3);            // TS
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0) : "memory");

    // CR4: OSFXSR (fxsave/fxrstor + SSE legal in any privilege level) and
    // OSXMMEXCPT (unmasked SSE exceptions come in as #XM, not #UD).
    uint32_t cr4;
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1u << 9) | (1u << 10);   // OSFXSR | OSXMMEXCPT
    __asm__ __volatile__("mov %0, %%cr4" : : "r"(cr4) : "memory");

    // Build the clean template on this core. At BSP init and AP bringup
    // no task is running on this CPU yet, so clobbering the live FPU
    // state here is safe — the first schedule() onto this core fxrstor's
    // a task image over it anyway.
    __asm__ __volatile__("fninit");
    __asm__ __volatile__("fxsave %0" : "=m"(*(uint8_t (*)[512])fpu_clean_state));

    fpu_enabled = 1;
    write_serial_string("[FPU] eager fxsave context switching enabled\n");
}

void fpu_task_init(uint8_t* region) {
    if (!fpu_enabled) return;
    memcpy(region, fpu_clean_state, 512);
}

void fpu_save(uint8_t* region) {
    if (!fpu_enabled) return;
    __asm__ __volatile__("fxsave %0" : "=m"(*(uint8_t (*)[512])region));
}

void fpu_restore(uint8_t* region) {
    if (!fpu_enabled) return;
    __asm__ __volatile__("fxrstor %0" : : "m"(*(uint8_t (*)[512])region));
}
