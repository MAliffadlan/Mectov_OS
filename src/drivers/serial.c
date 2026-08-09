#include "../include/serial.h"
#include "../include/io.h"
#include "../include/spinlock.h"

// Fase 3 (per-CPU scheduler): serial output now originates from every core, so
// a bare write interleaves byte-by-byte between CPUs and garbles every log
// line ("[TASK] fork: child tid=" becomes unreadable). The lock serializes
// whole write_serial_string/write_serial_hex calls. Interrupts are disabled
// around the lock so an IRQ handler logging mid-string cannot self-deadlock.
static spinlock_t serial_lock = SPINLOCK_INIT;

// Raw single-char write (no lock) — only call while holding serial_lock.
static void serial_putc_locked(char a) {
    int timeout = 100000;
    while (is_transmit_empty() == 0 && timeout > 0) timeout--;
    if (timeout > 0) outb(MODEM_PORT, a);
}

void init_serial() {
    outb(MODEM_PORT + 1, 0x00);    // Disable all interrupts
    outb(MODEM_PORT + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(MODEM_PORT + 0, 0x03);    // Set divisor to 3 (lo byte) 38400 baud
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
    for (int i = 0; i < size; i++) serial_putc_locked_raw(buf[i]);
    if (took) spin_unlock(&serial_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
}
