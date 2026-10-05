#!/bin/bash
# scripts/tls_selftest_host.sh -- run apps/tlsselftest.c on the host.
#
# The self-test is written for the guest: its only output call is sys_print(),
# which is the int $0x80 wrapper in src/include/syscall.h. To run the same file
# on the host we substitute that one name and link a shim that prints to stdout.
# Everything else -- the fixtures, the vectors, the assertions -- is the file
# that ships in the image, so a green run here means the guest assertions are
# checking the same bytes.
#
# This is a development aid. The gate that counts is the in-guest one:
#   make check-quick   (runs scripts/tls_selftest.py)
set -e

cd "$(dirname "$0")/.."
ROOT="$PWD"
OUT="${TMPDIR:-/tmp}/tlsselftest_host"

# _start is renamed because the host C runtime already owns that symbol.
sed -e 's/\bsys_print(/host_print(/g' \
    -e 's/\bsys_exit_with_code(/host_exit(/g' \
    -e 's/\b_start\b/host_start/g' \
    apps/tlsselftest.c > "$OUT.c"

cat > "$OUT.shim.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
// Mirrors sys_print(const char*, int) and sys_exit_with_code(int) in
// src/include/syscall.h. The colour is dropped: on the host the harness greps
// the text, not the escape codes. The guest enters at _start because the .mct
// loader jumps at the ELF entry symbol, so the shim calls it and turns the
// status into the process exit code, exactly as the syscall does in the guest.
void host_print(const char* s, int colour) { (void)colour; fputs(s, stdout); }
void host_start(void);
void host_exit(int code) { exit(code); }
int main(void) { host_start(); return 0; }
EOF

gcc -m32 -O2 -w -I"$ROOT" -I"$ROOT/apps" \
    -o "$OUT" "$OUT.c" "$OUT.shim.c" \
    apps/lib/tls/tls_hash.c apps/lib/tls/tls_cipher.c \
    apps/lib/tls/tls_pubkey.c apps/lib/tls/asn1_x509.c

"$OUT"
