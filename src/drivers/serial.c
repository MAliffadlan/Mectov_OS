#include "../include/serial.h"
#include "../include/io.h"
#include "../include/spinlock.h"
#include "../include/klog.h"

// Fase 3 (per-CPU scheduler): serial output now originates from every core, so
// a bare write interleaves byte-by-byte between CPUs and garbles every log
// line ("[TASK] fork: child tid=" becomes unreadable). The lock serializes
// whole write_serial_string/write_serial_hex calls. Interrupts are disabled
// around the lock so an IRQ handler logging mid-string cannot self-deadlock.
static spinlock_t serial_lock = SPINLOCK_INIT;

// ---- v38.157: asynchronous transmit ring ----
// Every byte used to be a THRE poll + a THRE wait + a port write, i.e. two
// PIO VM exits per byte, with interrupts disabled and serial_lock held. At
// 4 vCPUs the BQL turnaround makes an exit expensive: Q3's own diagnostics
// (~550 bytes per 20 frames) measured ~2.4 ms/frame inside other_ms and, far
// worse, ~47 ms of frozen guest every 20 frames — during which the mouse
// queue backed up and then arrived in one clump (the "patah-patah" report).
//
// So the writers now only append to this RAM ring; the UART is topped up
// opportunistically from the timer IRQ (serial_poll, called on every core's
// vector-32 handler). QEMU's 16550 (hw/char/serial.c) drains the THR/FIFO into
// the backend immediately — there is no baud pacing on transmit — so once
// THRE reads set the FIFO is empty and a whole 14-byte burst can be written
// from ONE poll (the FIFO we enable is 16 deep, and 14 is the trigger level
// programmed in init_serial). A real 8250 without a FIFO reports IIR FIFO=0
// and falls back to one byte per poll, exactly the old semantics.
//
// Ordering and completeness: one ring, appends serialized by serial_lock,
// one drainer at a time. If the ring is ever full (a stall longer than the
// 16 KB ring at 1.4 KB/s of diagnostics) the appender drains it synchronously
// instead of dropping or reordering bytes. Panic/exception paths
// (write_serial_try) flush the ring first so a dump can never appear before
// the lines that preceded it.
#define SER_TX_RING    16384
#define SER_TX_MASK    (SER_TX_RING - 1)
#define SER_TX_BURST   14
static char tx_ring[SER_TX_RING];
static volatile uint16_t tx_head, tx_tail;
static int serial_fifo_state = -1;   /* -1 unknown, 1 FIFO on, 0 no FIFO */

static int serial_fifo_on(void) {
    if (serial_fifo_state < 0)
        serial_fifo_state = ((inb(MODEM_PORT + 2) & 0xC0) == 0xC0) ? 1 : 0;
    return serial_fifo_state;
}

// Push queued bytes into the UART while THRE says it can take them. Never
// waits: one poll, then a burst (or a single byte without a FIFO). Returns
// the number of bytes moved.
static int serial_topup_locked(void) {
    if (tx_tail == tx_head) return 0;
    if (is_transmit_empty() == 0) return 0;
    int burst = serial_fifo_on() ? SER_TX_BURST : 1;
    int n = 0;
    while (n < burst && tx_tail != tx_head) {
        outb(MODEM_PORT, tx_ring[tx_tail]);
        tx_tail = (uint16_t)((tx_tail + 1) & SER_TX_MASK);
        n++;
    }
    return n;
}

// Synchronous drain of the whole ring (panic path and the full-ring fallback).
// Keeps the old dead-port guard: a port that never reports THRE drops the
// byte after the timeout instead of hanging the caller forever.
static int serial_drain_blocking_locked(void) {
    int moved = 0;
    while (tx_tail != tx_head) {
        int timeout = 100000;
        while (is_transmit_empty() == 0 && timeout > 0) timeout--;
        if (timeout <= 0) break;
        outb(MODEM_PORT, tx_ring[tx_tail]);
        tx_tail = (uint16_t)((tx_tail + 1) & SER_TX_MASK);
        moved++;
    }
    return moved;
}

// Raw single-char write (no lock) — only call while holding serial_lock.
// Every byte is also captured in the kernel log ring buffer (dmesg). Append
// to the transmit ring; the timer IRQ (or the string-level kick after a
// multi-byte write) moves it to the UART.
static void serial_putc_locked(char a) {
    klog_putc(a);
    uint16_t next = (uint16_t)((tx_head + 1) & SER_TX_MASK);
    if (next == tx_tail) serial_drain_blocking_locked();   /* ring full */
    tx_ring[tx_head] = a;
    tx_head = next;
}

void init_serial() {
    outb(MODEM_PORT + 1, 0x00);    // Disable all interrupts
    outb(MODEM_PORT + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    // v38.148: divisor 1 = 115200 baud, was 3 = 38400.
    // v38.157: the divisor no longer matters for stalls — writers do not wait
    // on the UART at all any more (see the transmit ring above); this stays at
    // the fastest setting for the panic path's direct writes.
    outb(MODEM_PORT + 0, 0x01);    // Set divisor to 1 (lo byte) 115200 baud
    outb(MODEM_PORT + 1, 0x00);    //                  (hi byte)
    outb(MODEM_PORT + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(MODEM_PORT + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
    outb(MODEM_PORT + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

int serial_received() {
    uint8_t status = inb(MODEM_PORT + 5);
    if (status == 0xFF) return 0; // Port is dead or unmapped
    return status & 1;
}

char read_serial() {
    int timeout = 100000;
    while (serial_received() == 0 && timeout > 0) timeout--;
    if (timeout > 0) return inb(MODEM_PORT);
    return 0;
}

int is_transmit_empty() {
    uint8_t status = inb(MODEM_PORT + 5);
    if (status == 0xFF) return 0; // Prevent infinite loop on dead port
    return status & 0x20;
}

void write_serial(char a) {
    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    spin_lock(&serial_lock);
    serial_putc_locked(a);
    spin_unlock(&serial_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
}

void write_serial_string(const char* str) {
    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    spin_lock(&serial_lock);
    for (int i = 0; str[i] != '\0'; i++) {
        serial_putc_locked(str[i]);
    }
    serial_topup_locked();
    spin_unlock(&serial_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
}

// Write a raw buffer in ONE locked call so a multi-byte message (e.g. an app
// writing to fd 1/2) cannot be split by another CPU's log lines.
void write_serial_buffer(const char* buf, int size) {
    if (!buf || size <= 0) return;
    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    spin_lock(&serial_lock);
    for (int i = 0; i < size; i++) {
        serial_putc_locked(buf[i]);
    }
    serial_topup_locked();
    spin_unlock(&serial_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
}

void write_serial_hex(uint32_t val) {
    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    spin_lock(&serial_lock);
    serial_putc_locked('0');
    serial_putc_locked('x');
    for (int i = 28; i >= 0; i -= 4) {
        int nibble = (val >> i) & 0xF;
        if (nibble < 10) serial_putc_locked('0' + nibble);
        else serial_putc_locked('A' + (nibble - 10));
    }
    serial_topup_locked();
    spin_unlock(&serial_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
}

// ---- Exception-context serial (deadlock-free) ----
// The #PF handler (COW promotion, guard-page overflow) runs on the dedicated
// fault stack with IF=0 and MUST be able to log even when the interrupted
// pre-exception context already holds serial_lock — otherwise an exception
// fired mid-log spins forever on its own lock and silently freezes every core
// (all of them cli + spin on serial_lock). A panic line garbled at the
// boundary beats a system that hangs with no output at all.
static void serial_putc_locked_raw(char a) {
    klog_putc(a);
    int timeout = 100000;
    while (is_transmit_empty() == 0 && timeout > 0) timeout--;
    if (timeout > 0) outb(MODEM_PORT, a);
}

// Write one atomic line if the lock is free; fall back to raw (unlocked)
// writes when it is not. Only the exception path calls this.
void write_serial_try(const char* buf, int size) {
    if (!buf || size <= 0) return;
    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    int took = spin_try_lock(&serial_lock);
    /* v38.157: flush what was queued BEFORE this line first — in a panic the
     * dump must not jump ahead of the log that led to it. When the lock is
     * held by a dead core we keep the raw unlocked fallback (completeness
     * beats ordering there, exactly as before). */
    if (took) serial_drain_blocking_locked();
    for (int i = 0; i < size; i++) serial_putc_locked_raw(buf[i]);
    if (took) spin_unlock(&serial_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
}

// Diagnostic variant for the exception path: writes the line ONLY when the
// serial lock is free, otherwise drops it entirely. Routine per-fault logs
// (COW page duplication, demand-paging) must not garble another core's
// locked line by interleaving raw bytes into it — under real SMP that turns
// every "fork: child tid=" marker into unreadable noise. Panic/error paths
// keep write_serial_try()'s raw fallback so they always print. Returns 1 if
// the line was written atomically, 0 if it was dropped.
int write_serial_if_free(const char* buf, int size) {
    if (!buf || size <= 0) return 0;
    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    int took = spin_try_lock(&serial_lock);
    if (!took) {
        __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
        return 0;
    }
    for (int i = 0; i < size; i++) serial_putc_locked(buf[i]);
    serial_topup_locked();
    spin_unlock(&serial_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
    return 1;
}

// v38.157: transmit-ring drain hook. Called from every core's vector-32
// handler (timer.c) ~100x/s: one THRE poll plus, when the FIFO is empty, a
// burst of up to 14 bytes. Non-blocking on purpose — a timer IRQ must never
// wait on the UART, and spin_try_lock keeps it deadlock-free when another
// core holds serial_lock. 4 cores x 100 Hz x up to 14 bytes is >=5.6 KB/s of
// drain capacity against the ~1.4 KB/s the Q3 diagnostics produce; the log
// lags by tens of ms instead of freezing the frame that wrote it.
void serial_poll(void) {
    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    if (spin_try_lock(&serial_lock)) {
        serial_topup_locked();
        spin_unlock(&serial_lock);
    }
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
}
