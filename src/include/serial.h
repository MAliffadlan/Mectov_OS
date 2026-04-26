#ifndef SERIAL_H
#define SERIAL_H

#include "types.h"

#define PORT_COM1 0x3F8
#define PORT_COM2 0x2F8
#define MODEM_PORT PORT_COM2

void init_serial();
int serial_received();
char read_serial();
int is_transmit_empty();
void write_serial(char a);
void write_serial_string(const char* str);

#endif
