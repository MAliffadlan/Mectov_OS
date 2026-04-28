#include "../include/serial.h"
#include "../include/io.h"

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
    int timeout = 100000;
    while (is_transmit_empty() == 0 && timeout > 0) timeout--;
    if (timeout > 0) outb(MODEM_PORT, a);
}

void write_serial_string(const char* str) {
    for (int i = 0; str[i] != '\0'; i++) {
        write_serial(str[i]);
    }
}

void write_serial_hex(uint32_t val) {
    write_serial('0');
    write_serial('x');
    for (int i = 28; i >= 0; i -= 4) {
        int nibble = (val >> i) & 0xF;
        if (nibble < 10) write_serial('0' + nibble);
        else write_serial('A' + (nibble - 10));
    }
}
