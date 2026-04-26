#include "../include/utils.h"
#include "../include/io.h"
#include "../include/vga.h"

char cpu_brand[49];
unsigned char boot_sec, boot_min, boot_hour;
unsigned int rand_seed = 0;

unsigned char read_cmos(unsigned char reg) { outb(0x70, reg); return inb(0x71); }
unsigned char bcd_to_bin(unsigned char bcd) { return ((bcd & 0xF0) >> 1) + ((bcd & 0xF0) >> 3) + (bcd & 0x0F); }

void detect_cpu() {
    unsigned int eax, ebx, ecx, edx;
    unsigned int *ptr = (unsigned int *)cpu_brand;
    for (unsigned int i = 0; i < 3; i++) {
        __asm__ __volatile__ ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000002 + i));
        ptr[i * 4 + 0] = eax; ptr[i * 4 + 1] = ebx; ptr[i * 4 + 2] = ecx; ptr[i * 4 + 3] = edx;
    }
    cpu_brand[48] = '\0';
}

void init_uptime() { boot_sec = bcd_to_bin(read_cmos(0x00)); boot_min = bcd_to_bin(read_cmos(0x02)); boot_hour = bcd_to_bin(read_cmos(0x04)); }

void shutdown() { outw(0x604, 0x2000); }
void reboot() { unsigned char g = 0x02; while (g & 0x02) g = inb(0x64); outb(0x64, 0xFE); }

int strcmp(const char* s1, const char* s2) { while (*s1 && (*s1 == *s2)) { s1++; s2++; } return *(const unsigned char*)s1 - *(const unsigned char*)s2; }
int strncmp(const char* s1, const char* s2, int n) { while (n && *s1 && (*s1 == *s2)) { ++s1; ++s2; --n; } if (n == 0) return 0; return (*(unsigned char *)s1 - *(unsigned char *)s2); }
void strcpy(char* d, const char* s) { while ((*d++ = *s++)); }

void p_int(int n, unsigned char c) { if (n < 0) { print("-", c); n = -n; } if (n == 0) { print("0", c); return; } char buf[10]; int i = 0; while (n > 0) { buf[i++] = (n % 10) + '0'; n /= 10; } for (int j = 0; j < i / 2; j++) { char t = buf[j]; buf[j] = buf[i - j - 1]; buf[i - j - 1] = t; } buf[i] = '\0'; print(buf, c); }

int atoi(const char* s) { int r = 0, si = 1, i = 0; if (s[0] == '-') { si = -1; i++; } for (; s[i] != '\0'; ++i) { if (s[i] >= '0' && s[i] <= '9') r = r * 10 + s[i] - '0'; else break; } return si * r; }

unsigned int rand() { rand_seed = rand_seed * 1103515245 + 12345; return (unsigned int)(rand_seed / 65536) % 32768; }
