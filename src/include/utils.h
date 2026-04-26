#ifndef UTILS_H
#define UTILS_H

#include "types.h"

extern char cpu_brand[49];
extern unsigned char boot_sec, boot_min, boot_hour;
extern unsigned int rand_seed;

unsigned char read_cmos(unsigned char reg);
unsigned char bcd_to_bin(unsigned char bcd);
void detect_cpu();
void init_uptime();
void shutdown();
void reboot();

int strlen(const char* s);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, int n);
void strcpy(char* d, const char* s);
void memset(void* dest, uint8_t val, uint32_t len);
void memcpy(void* dest, const void* src, uint32_t len);
void p_int(int n, unsigned char c);
int atoi(const char* s);
unsigned int rand();

#endif
