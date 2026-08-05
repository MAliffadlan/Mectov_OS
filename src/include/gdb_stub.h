#ifndef GDB_STUB_H
#define GDB_STUB_H

#include "idt.h"

// In-kernel GDB Remote Serial Protocol stub (COM2, 38400 baud).
//
// Activate from the running system by pressing F12 (kernel.c hooks scancode
// 0x58) or by placing an `int3` in kernel code. When active, the kernel freezes
// inside an ISR and speaks GDB RSP until you 'continue'.
//
// Usage with QEMU (COM2 = second -serial):
//   qemu ... -serial tcp:127.0.0.1:2345,server=on,wait=off
//   gdb myos.bin -ex "target remote :2345" -ex "c"
// Then press F12 in the guest; gdb stops, you can set breakpoints with
// `break kernel_main` etc. Software breakpoints (Z0/z0) are supported.

void gdb_stub_init(void);

// Called from isr_handler() for exceptions 1 (single-step) and 3 (breakpoint).
// Returns 1 when the trap was consumed by the debugger (do not run the normal
// exception path), 0 when GDB is not attached / not initialized.
int gdb_stub_handle_trap(registers_t* r);

// Called from the kernel main loop every iteration: answers the GDB connect
// handshake (`target remote`) while the OS is running, so you can attach
// before pressing F12.
void gdb_stub_poll(void);

// Drop into the debugger right now (F12). Implemented as an int3.
void gdb_stub_break(void);

#endif
