#ifndef ENTROPY_H
#define ENTROPY_H

#include "types.h"
#include "spinlock.h"

// ---- Kernel entropy pool + CSPRNG (hardening step 2) ----
//
// The old /dev/random was a fixed-seed LCG; TCP ISNs were derived from
// get_ticks()*constant, which is predictable. This module keeps a small
// entropy pool fed from the timing jitter of hardware events (IRQ arrival
// skew, TSC low bits, and a counter-embedded SipHash-style mixer) and stretches
// it through a ChaCha8-based DRBG. Used by /dev/random, SYS_GETRANDOM,
// password salt generation, ASLR offsets and TCP initial sequence numbers.
//
// ChaCha (8 rounds) as the expansion function: 12 rounds is the conservative
// recommendation, but every round keeps the speed reasonable for the 1 kHz timer
// path and the 32-bit core this runs on; the input entropy is the real source
// of strength here, and the attacker model is "predict the next ISN/salt", not
// "break ChaCha".

void entropy_init(void);

// Mix one word + a counter into the pool. Cheap, called from the timer IRQ and
// keyboard/mouse paths with the captured IRQ timing jitter. Safe from any
// context (short spinlock, IF already 0 on the IRQ path).
void entropy_add(uint32_t counter);

// Fill buf with n cryptographically-random bytes. Returns 0 on success, -1 if
// the pool has never been seeded (entropy_init not called).
int get_random_bytes(void* buf, uint32_t n);

// One 32-bit random value. Panics? No — returns 0 if unseeded (callers that
// need real randomness should check, but 0 is safe and obvious).
uint32_t get_random_u32(void);

#endif
