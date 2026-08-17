#ifndef SHA256_H
#define SHA256_H

#include "types.h"

// SHA-256 (FIPS 180-4), self-contained — no libc, no heap.
// Used by the password subsystem (src/sys/passwd.c) to store salted hashes
// instead of plain text in /etc/passwd.

typedef struct {
    uint32_t state[8];
    uint64_t total_len;      // bytes consumed so far (shifting byte counter)
    uint8_t  block[64];
    uint32_t block_len;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t* c);
void sha256_update(sha256_ctx_t* c, const void* data, uint32_t len);
void sha256_final(sha256_ctx_t* c, uint8_t out[32]);

// Convenience one-shot.
void sha256_once(const void* data, uint32_t len, uint8_t out[32]);

#endif
