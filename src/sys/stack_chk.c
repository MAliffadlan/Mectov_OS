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
