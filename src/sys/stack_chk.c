#include "../include/types.h"
#include "../include/serial.h"

// Referenced by every function GCC instruments with -fstack-protector-strong
// (-mstack-protector-guard=global keeps it out of TLS, which this kernel does
// not set up). boot.asm seeds it from RDTSC before calling kernel_main; this
// initializer only covers a loader that bypasses boot.asm.
uintptr_t __stack_chk_guard = 0x4D454354u;   // 'MECT'

__attribute__((noreturn)) void __stack_chk_fail(void) {
    write_serial_string("\n[KERNEL PANIC] STACK SMASHING DETECTED\n");
    extern void panic_finish(void);
    panic_finish();
    for (;;) __asm__ __volatile__("hlt");
}

// CI probe: noinline keeps the array in its own frame, so 20 bytes written
// into a 16-byte buffer overwrite the saved canary and the return check calls
// __stack_chk_fail.
//
// The bound and the destination are both laundered through volatile: written
// with a literal `i < 20` GCC constant-folds the loop, proves the last four
// iterations are UB, and reports it (-Waggressive-loop-optimizations) plus
// "set but not used" for the buffer. The overflow is the entire point of this
// function, so it is made opaque rather than suppressed — the build stays at
// zero warnings and the probe still trips the canary.
__attribute__((noinline)) void stack_chk_self_test(void) {
    volatile char buf[16];
    volatile int n = 20;                 // opaque bound: not foldable
    volatile char* p = buf;
    for (int i = 0; i < n; i++) p[i] = (char)i;
}
