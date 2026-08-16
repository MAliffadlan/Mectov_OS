#ifndef MSR_H
#define MSR_H

#include "types.h"

// Model-specific register helpers (v38.49, PAE/NX bring-up). No MSR code
// existed in the tree before — LAPIC access is MMIO-only.
static inline void wrmsr(uint32_t msr, uint64_t value) {
    __asm__ __volatile__("wrmsr"
        : : "a"((uint32_t)value), "d"((uint32_t)(value >> 32)), "c"(msr));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

#define MSR_EFER   0xC0000080
#define EFER_NXE   (1ULL << 11)   // NXE: bit 63 of page entries = no-execute

#endif
