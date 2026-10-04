#ifndef SERIAL_H
#define SERIAL_H

#include "types.h"

#define PORT_COM1 0x3F8
#define PORT_COM2 0x2F8
#define MODEM_PORT PORT_COM1

void init_serial();
int serial_received();
char read_serial();
int is_transmit_empty();
void write_serial(char a);
void write_serial_string(const char* str);
void write_serial_buffer(const char* buf, int size);
void write_serial_hex(uint32_t val);
// Exception-context write: takes serial_lock if free, otherwise writes raw
// (deadlock-free). Only for the #PF/#DF/exception paths.
void write_serial_try(const char* buf, int size);
int  write_serial_if_free(const char* buf, int size);
// v38.157: non-blocking transmit-ring drain, called from every core's timer
// tick (see serial.c). Pushes queued log bytes into the UART FIFO without
// waiting, so a writer never stalls the frame it is logging from.
void serial_poll(void);

#endif
