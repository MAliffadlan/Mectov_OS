# CMOS Real-Time Clock (MC146818)

The kernel's only wall clock. `src/drivers/rtc.c` reads the battery-backed
CMOS chip through the two I/O ports every PC has had since 1984, and
`SYS_GET_TIME` is how Ring 3 sees it.

---

## 🎯 Scope (v38.165)

* **Chip**: MC146818-compatible RTC at ports `0x70` (index, bit 7 = NMI mask)
  and `0x71` (value). Registers used: `0x00` sec, `0x02` min, `0x04` hour,
  `0x06` day-of-week, `0x07` day, `0x08` month, `0x09` year, `0x0A` status A,
  `0x0B` status B, `0x32` century (optional).
* **Format**: BCD *or* binary, 12-hour *or* 24-hour — selected by Status
  Register B, not assumed.
* **Readers**: `SYS_GET_TIME` (any task, any core), the taskbar clock, the
  login lockout, and the timer's once-per-second rate calibration. Four
  concurrent readers is why this driver is a lock and not a comment.
* **Gates**: `scripts/rtc_test.py` (guest, real SMP) and
  `scripts/rtc_decode_test.py` (host, deterministic).

---

## ⚠️ Why a register read is not one read

The chip has **one** index register shared by every reader. A value read is
therefore a two-port transaction that the *chip* does not make atomic:

```
  outb(0x70, 0x80 | reg)   <-- selects a register, globally
  inb(0x71)                <-- returns whatever is selected NOW
```

Two CPUs can interleave those pairs, and then `inb(0x71)` returns the register
the *other* CPU selected. The v38.165 measurement, four threads on four cores
calling `SYS_GET_TIME` (`apps/rtc_stress.c`):

| | samples | torn |
|---|---|---|
| before the lock | 48 000 | **32 775 – 36 755** |
| after the lock | 144 000 (3 rounds) | **0** |

"Torn" means the returned time was assembled from registers from different
instants — an hour of 40, a month of 26, a year of 1530, a day-of-week of 0.
`cmos_read()` is now only callable with `rtc_lock` held (`cmos_read_locked`),
taken with `spin_lock_irqsave` in process context (the house rule in
`src/include/spinlock.h`), and every reader — including `rtc_init()`'s century
probe — goes through it.

## ⚠️ Why the lock is not enough: the update cycle

Once per second the chip copies its internal counters into the registers and
raises **UIP** in Status Register A about 244 µs early. A burst of eight
register reads takes microseconds, so an update can begin *inside* it and the
stamp then straddles two instants even on a single core.

The burst is therefore bracketed by two Status-A reads, and retried when
either shows UIP (`RTC_READ_ATTEMPTS = 4`). The wait for UIP to clear happens
**outside** the lock: it can take milliseconds, and holding a lock with IF=0
that long is exactly the v38.157 "cli for seconds" failure mode.

A burst that is unstable across every attempt is logged once
(`[RTC] no stable stamp after 0x… attempts (regA=0x…)`, throttled) and the last
image is decoded best-effort — nothing is invented, and both gates would see
it.

## ⚠️ Status Register B was never read

The previous driver applied `bcd2bin()` to every field, forever. That is only
right for one of the four combinations the chip supports:

| Status B | field | meaning |
|---|---|---|
| bit 2 `DM` = 0 | BCD | `0x59` is 59 |
| bit 2 `DM` = 1 | binary | `0x59` is 89 |
| bit 1 `24/12` = 1 | 24-hour | hour `0x00`–`0x23` |
| bit 1 `24/12` = 0 | 12-hour | hour `0x01`–`0x12`, **bit 7 = PM**; 12 AM is hour 0 |

QEMU/SeaBIOS is BCD + 24-hour, so **no guest suite could ever see this** — the
whole battery passed while the driver was wrong on any machine that is not.
`rtc_decode_regs()` now takes Status B with the image, which also makes the
rule testable on the host (below).

## 🔌 Where the decode lives

`rtc_decode_regs(const unsigned char* regs, rtc_time_t* out)` — declared in
`src/include/rtc.h`, indexed by the `RTC_R_*` enum — is a pure function of the
register image. It returns `-1` when the image cannot be a calendar time, which
is what turns a crossed read into a retry (and into a detectable event) instead
of a plausible-looking stamp. `rtc_read_time()` is the port-touching wrapper:
wait for UIP, burst under the lock, decode, retry on instability.

---

## 🧪 The two gates, and what each one can and cannot prove

| Gate | Kind | Proves |
|---|---|---|
| `scripts/rtc_test.py` | guest, `-smp 4`, `apps/rtc_stress.mct` | the **lock**: four threads, 48 000 samples per round, `PASS checked=N refused=0 unwritten=0`. RED before the fix: `FAIL torn=32775 of 48000`. |
| `scripts/rtc_decode_test.py` | host, no QEMU (≈1 s) | the **decode rules** (binary, 12-hour PM, 12-hour 12 AM) and the **straddle retry**: it compiles the shipped `src/drivers/rtc.c` against a fake chip (`scripts/rtc_host/`) that can swap the image mid-burst and raise UIP on a chosen Status-A read. RED 5/8 before the fix, 8/8 after. |

The host harness needs the driver's includes rewritten to the shim headers; it
does that in a temp directory, so the decode rules and the port sequence under
test are the shipped ones. The RED proof is therefore available at any time:

```sh
git show HEAD~1:src/drivers/rtc.c > /tmp/rtc_old.c     # or any earlier revision
python3 scripts/rtc_decode_test.py --driver /tmp/rtc_old.c
```

The detector counts three outcomes **separately**, because they are different
bugs: `checked` (a stamp pair that agreed), `torn` (both calls succeeded, the
stamp is impossible), `refused` (`SYS_GET_TIME` returned non-zero) and
`unwritten` (the call reported success but the buffer still held the pre-fill
sentinel). A sentinel pre-fill is what makes `unwritten` detectable at all; the
verdict fails on any of the three.

## 📌 Limits, stated rather than implied

* **A tear that swaps two registers holding the SAME value is not detectable**
  by `rtc_stress` (the stamp stays self-consistent). The gate is a detector for
  observable inconsistency, not a proof that the pair is serialized; the lock
  is justified by the mechanism and by the 32k→0 measurement.
* **The host harness cannot prove the lock** — it is single-threaded, so the
  shim's lock is a no-op. Each gate covers what the other cannot.
* **NMI stays disabled** after the first access (bit 7 of `0x70` latches): the
  driver sets it on every index write and never clears it.
* **The century register is optional.** A value that is not a plausible
  century falls back to the one `rtc_init()` probed (default 20xx), so a chip
  without `0x32` reads as 20xx.
* **No alarm interrupts, no periodic-interrupt rate programming**, no `SET`
  (bit 7 of Status B): the driver only reads.
