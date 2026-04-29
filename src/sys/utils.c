#include "../include/utils.h"
#include "../include/io.h"
#include "../include/vga.h"
#include "../include/rtc.h"

char cpu_brand[49];
unsigned char boot_sec, boot_min, boot_hour;
unsigned int rand_seed = 0;

// Legacy CMOS read — NMI-safe version (bit 7 set to disable NMI)
unsigned char read_cmos(unsigned char reg) { outb(0x70, reg | 0x80); return inb(0x71); }
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

void init_uptime() {
    rtc_init();
    rtc_time_t bt = rtc_read_time();
    boot_sec = bt.second;
    boot_min = bt.minute;
    boot_hour = bt.hour;
}

void shutdown() {
    print("Shutting down...\n", 0x0C);
    __asm__ volatile("cli");
    outw(0xB004, 0x2000);   // ACPI PM1a_CNT (QEMU PIIX4)
    outw(0x604, 0x2000);    // QEMU isa-debug-exit
    outw(0x4004, 0x3400);   // VirtualBox fallback
    // If all fail, halt CPU forever
    for(;;) __asm__("hlt");
}
void reboot() {
    print("Rebooting...\n", 0x0E);
    __asm__ volatile("cli");
    // Keyboard controller 8042 reset
    unsigned char g;
    do { g = inb(0x64); } while (g & 0x02);
    outb(0x64, 0xFE);
    // Fallback: halt if reset fails
    for(;;) __asm__("hlt");
}

int strlen(const char* s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

int strcmp(const char* s1, const char* s2) {
 while (*s1 && (*s1 == *s2)) { s1++; s2++; } return *(const unsigned char*)s1 - *(const unsigned char*)s2; }
int strncmp(const char* s1, const char* s2, int n) { while (n && *s1 && (*s1 == *s2)) { ++s1; ++s2; --n; } if (n == 0) return 0; return (*(unsigned char *)s1 - *(unsigned char *)s2); }
void strcpy(char* d, const char* s) { while ((*d++ = *s++)); }

void memset(void* dest, uint8_t val, uint32_t len) {
    // Fill 4 bytes at a time using rep stosl
    uint32_t fill = val | (val << 8) | (val << 16) | (val << 24);
    uint32_t dwords = len / 4;
    uint32_t rem = len % 4;
    __asm__ __volatile__(
        "rep stosl"
        : "+D"(dest), "+c"(dwords)
        : "a"(fill)
        : "memory"
    );
    uint8_t* d8 = (uint8_t*)dest;
    while (rem--) *d8++ = val;
}

void memcpy(void* dest, const void* src, uint32_t len) {
    uint32_t dwords = len / 4;
    uint32_t rem = len % 4;
    
    __asm__ __volatile__(
        "rep movsd\n\t"
        "movl %3, %%ecx\n\t"
        "rep movsb"
        : "+D"(dest), "+S"(src)
        : "c"(dwords), "g"(rem)
        : "memory"
    );
}

void p_int(int n, unsigned char c) { if (n < 0) { print("-", c); n = -n; } if (n == 0) { print("0", c); return; } char buf[10]; int i = 0; while (n > 0) { buf[i++] = (n % 10) + '0'; n /= 10; } for (int j = 0; j < i / 2; j++) { char t = buf[j]; buf[j] = buf[i - j - 1]; buf[i - j - 1] = t; } buf[i] = '\0'; print(buf, c); }

int atoi(const char* s) { int r = 0, si = 1, i = 0; if (s[0] == '-') { si = -1; i++; } for (; s[i] != '\0'; ++i) { if (s[i] >= '0' && s[i] <= '9') r = r * 10 + s[i] - '0'; else break; } return si * r; }

unsigned int rand() { rand_seed = rand_seed * 1103515245 + 12345; return (unsigned int)(rand_seed / 65536) % 32768; }
