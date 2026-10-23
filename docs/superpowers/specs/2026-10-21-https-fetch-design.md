# Shell `fetch https://` — TLS 1.3 fetch in the shell (2026-10-21)

## Context

Mectov OS already ships a complete, audit-grade TLS 1.3 client (`apps/lib/tls/`:
ChaCha20-Poly1305 + AES-128-GCM, X.509 chain verify against a 121-CA trust store built
by `scripts/gen_tls_roots.py`, hostname/SAN matching, non-blocking `tls_step()` state
machine with `tls_set_transport()`). The GUI browser (`apps/browser.c`) already drives
it end-to-end for `https://` URLs — URL parse (`https://` → port 443), `sys_tcp_connect`,
TLS transport callbacks (`apps/browser.c:167-183`), non-blocking `tls_step` loop.

The gap: the **shell `fetch` command** (`src/sys/shell/builtins/net_cmds/cmd_fetch_arg.c`)
is HTTP-only — hard-coded port 80, plain TCP, no URL parsing. Today an HTTPS fetch is only
possible from the browser window.

**Goal:** `fetch example.com` / `fetch https://example.com/path` in the shell performs a
real, certificate-verified TLS 1.3 fetch and prints the response (headers + body) to the
terminal. Make it a user-space **.mct app** that the shell runs — because the TLS engine
links cleanly into .mct builds (like `tlsselftest.mct`) and a shell builtin lives in the
kernel binary where TLS does not (and should not) link.

## Requirements

1. `fetch <url>` where `<url>` is `https://host[:port][/path]`, `http://host[:port][/path]`,
   or bare `host` → defaults to **https** (modern default; `http://` remains explicit).
   Bare `host:443/path` → https (matches the browser's rule).
2. Full TLS 1.3 handshake with X.509 chain verification against the existing 121-CA store
   and hostname validation (reuse `tls_verify_chain` / the engine's built-in checks — no
   new crypto).
3. Send `GET <path> HTTP/1.1`, `Host:`, `Connection: close`, `User-Agent: Mectov/1.0`.
4. Print response headers, then body; print a short TLS status line first
   (`[TLS] … cipher, cert verified`).
5. Bounded: DNS timeout ~3 s, TCP connect ~5 s, TLS handshake ~10 s, body drain ~15 s
   idle (same bounds family as the HTTP fetch).
6. Non-fatal failures produce clear errors: `TLS certificate not trusted`,
   `TLS protocol error`, timeouts. Never hang the shell.

## Design

### One new file: `apps/fetch.c` ( a .mct app)
Merge the two existing patterns:
- **URL parse + connect + linear drive loop** from `cmd_fetch_arg.c` (bounded waits with
  `get_ticks` + `NET_WAIT_MAX_SPINS`, drain to EOF),
- **TLS transport glue + `tls_step` loop** from `apps/browser.c:167-183` + drive loop,
  but *blocking-friendly for the shell*: loop `tls_step()` until `TLS_DONE`, pump
  `sys_tcp_recv` on `TLS_WANT_READ`, `sys_tcp_send` on `TLS_WANT_WRITE`, bounded by ticks.

Flow:
1. Parse URL (scheme, host, port, path) — mirror `apps/browser.c:235+` rules exactly.
2. `sys_dns_resolve(host)` (bounded wait, 3 s).
3. `sys_tcp_connect(ip, port)` (port 443 for https, 80 for http).
4. For https: `tls_init(&c, host)`; `tls_set_transport(&c, fetch_tls_send, fetch_tls_recv,
   0, fetch_tls_log)`; then pump `tls_step()` with tick-bounded deadlines; on success send
   the GET request via `tls_write`, print `[TLS] <cipher> cert verified`.
   For http: send the request directly on the TCP conn (same as today's fetch).
5. Drain loop: read via `tls_read` (or raw `sys_tcp_recv` for http) to EOF / bounded idle,
   print to terminal via `sys_print` in chunks (terminal is line-oriented; keep chunks ≤ 512
   bytes and NUL-safe).

Reuse without modification:
- `apps/lib/tls/*` — engine, roots, verify (unchanged; pulled in by the .mct build).
- `sys_tcp_*` / `sys_dns_resolve` stubs from `src/include/syscall.h`.
- The Makefile `TLS_LIB_SRCS` group for linking (same as `tlsselftest.mct`).

### Shell plumbing: tmabah alias in the shell
`src/sys/shell/builtins/net_cmds/cmd_fetch_arg.c` gets a thin prefix-check: if the arg
starts with "https://", print a pointer to run the app (`fetch.mct`) via the existing
`run` path instead of the plain-HTTP builtin path. (Running another .mct from the shell
already exists via `cmd_run`; the alias keeps `fetch x` working for HTTP today and defers
https to the app.)

### Build wiring (Makefile)
```
fetch.mct: apps/fetch.c $(TLS_LIB_SRCS) $(TLS_LIB_HDRS) $(MCT_LIBC_H)
	python3 scripts/build_mct.py apps/fetch.c $(TLS_LIB_SRCS) fetch.mct
```

## Non-goals
- No HTTP chunked-encoding detail — print raw response bytes (headers+body) like
  today's fetch; no HTML rendering (that is the browser's job).
- No session resumption / ticket caching.
- No new cipher suites or crypto — engine untouched.
- No proxy support.

## Verification

1. `make fetch.mct` builds clean; `make` still 0 warnings.
2. Boot QEMU with the seed disk; in the shell: `run fetch.mct https://example.com`
   — expect: DNS resolve, TCP 443, `[TLS] cert verified`, HTTP response printed.
3. Negative test: `run fetch.mct https://self-signed.badssl` style host (or any host
   whose chain fails) → `[TLS] certificate not trusted` and a clean failure, no hang.
4. Plain HTTP regression: builtin `fetch example.com` (port 80) still works unchanged.
5. No new kernel paths: `dmesg` shows nothing new from the kernel network module beyond
   the usual TCP lifecycle.
