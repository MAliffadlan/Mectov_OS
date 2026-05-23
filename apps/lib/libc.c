#include "libc.h"

// --- Standard Libc Implementations ---

int impl_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

char* impl_strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

char* impl_strcat(char* dest, const char* src) {
    char* d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

void impl_itoa(int n, char* buf) {
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    int i = 0, sign = 0;
    if (n < 0) { sign = 1; n = -n; }
    while (n > 0) { buf[i++] = (n % 10) + '0'; n /= 10; }
    if (sign) buf[i++] = '-';
    buf[i] = '\0';
    for (int j = 0; j < i / 2; j++) {
        char t = buf[j]; buf[j] = buf[i - 1 - j]; buf[i - 1 - j] = t;
    }
}

void impl_itoa_pad(int n, char* buf, int pad) {
    impl_itoa(n, buf);
    int len = impl_strlen(buf);
    if (len < pad) {
        int diff = pad - len;
        for (int i = len; i >= 0; i--) {
            buf[i + diff] = buf[i];
        }
        for (int i = 0; i < diff; i++) {
            buf[i] = '0';
        }
    }
}

static void impl_xtoa(unsigned int n, char* buf) {
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    int i = 0;
    while (n > 0) {
        int rem = n % 16;
        if (rem < 10) buf[i++] = rem + '0';
        else buf[i++] = rem - 10 + 'A';
        n /= 16;
    }
    buf[i] = '\0';
    for (int j = 0; j < i / 2; j++) {
        char t = buf[j]; buf[j] = buf[i - 1 - j]; buf[i - 1 - j] = t;
    }
}

int impl_atoi(const char* s) {
    int res = 0;
    int sign = 1;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        res = res * 10 + (*s - '0');
        s++;
    }
    return res * sign;
}

// Simple LCG PRNG
static unsigned long int next = 1;
int impl_rand() {
    next = next * 1103515245 + 12345;
    return (unsigned int)(next / 65536) % 32768;
}

void impl_sprintf(char* buf, const char* format, void* vargs) {
    int* args = (int*)vargs;
    int arg_idx = 0;
    char temp[32];
    
    while (*format) {
        if (*format == '%') {
            format++;
            int pad_zero = 0;
            int width = 0;
            int left_align = 0;
            
            if (*format == '-') {
                left_align = 1;
                format++;
            }
            if (*format == '0') {
                pad_zero = 1;
                format++;
            }
            
            while (*format >= '0' && *format <= '9') {
                width = width * 10 + (*format - '0');
                format++;
            }

            if (*format == 'd' || *format == 'u') {
                if (width > 0 && pad_zero) {
                    impl_itoa_pad(args[arg_idx++], temp, width);
                } else {
                    impl_itoa(args[arg_idx++], temp);
                }
                int len = impl_strlen(temp);
                if (width > 0 && !left_align && len < width) {
                    for (int i = 0; i < width - len; i++) *buf++ = ' ';
                }
                impl_strcpy(buf, temp);
                buf += len;
                if (width > 0 && left_align && len < width) {
                    for (int i = 0; i < width - len; i++) *buf++ = ' ';
                }
            } else if (*format == 'x' || *format == 'X') {
                impl_xtoa((unsigned int)args[arg_idx++], temp);
                int len = impl_strlen(temp);
                if (width > 0 && pad_zero && len < width) {
                    for (int i = 0; i < width - len; i++) *buf++ = '0';
                } else if (width > 0 && !left_align && len < width) {
                    for (int i = 0; i < width - len; i++) *buf++ = ' ';
                }
                impl_strcpy(buf, temp);
                buf += len;
                if (width > 0 && left_align && len < width) {
                    for (int i = 0; i < width - len; i++) *buf++ = ' ';
                }
            } else if (*format == 's') {
                char* s = (char*)args[arg_idx++];
                if (!s) s = "(null)";
                int len = impl_strlen(s);
                if (width > 0 && !left_align && len < width) {
                    for (int i = 0; i < width - len; i++) *buf++ = ' ';
                }
                impl_strcpy(buf, s);
                buf += len;
                if (width > 0 && left_align && len < width) {
                    for (int i = 0; i < width - len; i++) *buf++ = ' ';
                }
            } else if (*format == 'c') {
                *buf++ = (char)args[arg_idx++];
            } else if (*format == '%') {
                *buf++ = '%';
            }
        } else {
            *buf++ = *format;
        }
        format++;
    }
    *buf = '\0';
}

void impl_printf(const char* format, void* vargs) {
    char buf[256];
    impl_sprintf(buf, format, vargs);
    sys_print(buf, 0x07); // Output to terminal with default color
}

// --- The Export Table ---
// We place this in a specific section so the linker puts it at the VERY BEGINNING of the binary.

__attribute__((section(".export_table")))
struct {
    char magic[4]; // "MLIB"
    uint32_t num_exports;
    void* exports[9]; // Increased to 9
} export_table = {
    "MLIB",
    9,
    {
        (void*)impl_strlen,
        (void*)impl_strcpy,
        (void*)impl_strcat,
        (void*)impl_itoa,
        (void*)impl_itoa_pad,
        (void*)impl_atoi,
        (void*)impl_rand,
        (void*)impl_printf,
        (void*)impl_sprintf
    }
};

// Dummy start function so the linker doesn't complain
void _start() {
    // A library should never be executed directly.
    sys_exit();
}
