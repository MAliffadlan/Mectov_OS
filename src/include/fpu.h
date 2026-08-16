#ifndef FPU_H
#define FPU_H

#include "types.h"

// Eager x87/SSE/MMX context switching (v38.41). Every task carries a
// 512-byte fxsave image in its task_t; schedule() swaps it on every
// context switch, so any number of FPU-using tasks (DOOM, Ring 3 float
// code) can preempt each other without corrupting each other's state.
// The kernel core itself stays soft-float (Makefile) and never emits FPU
// instructions of its own.

// Per-CPU bring-up: CR0 (EM clear, MP set, TS clear) + CR4 (OSFXSR |
// OSXMMEXCPT) + a CPUID feature check. Called by the BSP in kernel_main
// and by every AP in ap_main (CR4 is per-CPU, not inherited). Also builds
// the clean-state template on this core's FPU.
void fpu_init_cpu(void);

// Seed a fresh task image with the canonical clean state (fninit'd x87,
// default MXCSR — every exception masked). Used at task creation; exec
// also resets to it (POSIX: exec clears the FPU).
void fpu_task_init(uint8_t* fxsave_region);

// Swap helpers — region must be 16-byte aligned (task_t's field is; fxsave
// #GP's on misaligned operands). No-ops on a CPU without SSE.
void fpu_save(uint8_t* fxsave_region);
void fpu_restore(uint8_t* fxsave_region);

#endif
