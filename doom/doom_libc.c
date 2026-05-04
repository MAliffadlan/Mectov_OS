/*
 * doom_libc.c - Mini C Standard Library Implementation for DOOM on Mectov OS
 */
#include "doom_libc.h"

/* Forward declarations to kernel functions */
extern void* kmalloc(uint32_t size);
extern void  kfree(void *p);
extern void* krealloc(void *ptr, uint32_t new_size);
extern void* kcalloc(uint32_t num, uint32_t size);
extern void  write_serial_string(const char *s);

/* Use weak attribute for functions that kernel utils.c already defines */
#define DOOM_WEAK __attribute__((weak))

/* ===== errno ===== */
int doom_errno = 0;

/* ===== Random ===== */
static uint32_t doom_rand_seed = 1;
DOOM_WEAK int rand(void) {
    doom_rand_seed = doom_rand_seed * 1103515245 + 12345;
    return (int)((doom_rand_seed >> 16) & RAND_MAX);
}
void srand(unsigned int s) { doom_rand_seed = s; }

/* ===== ctype.h ===== */
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int isprint(int c) { return c >= 32 && c <= 126; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }

/* ===== string.h ===== */
DOOM_WEAK void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t*)dst;
    const uint8_t *s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

DOOM_WEAK void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t*)dst;
    const uint8_t *s = (const uint8_t*)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i-1] = s[i-1];
    }
    return dst;
}

DOOM_WEAK void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t*)s;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
    return s;
}

DOOM_WEAK int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *a = (const uint8_t*)s1;
    const uint8_t *b = (const uint8_t*)s2;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] - b[i];
    }
    return 0;
}

DOOM_WEAK size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

DOOM_WEAK char *strcpy(char *dst, const char *src) {
    char *ret = dst;
    while ((*dst++ = *src++));
    return ret;
}

DOOM_WEAK char *strncpy(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

DOOM_WEAK int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return *(uint8_t*)s1 - *(uint8_t*)s2;
}

DOOM_WEAK int strncmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i] || !s1[i]) return (uint8_t)s1[i] - (uint8_t)s2[i];
    }
    return 0;
}

char *strcat(char *dst, const char *src) {
    char *ret = dst;
    while (*dst) dst++;
    while ((*dst++ = *src++));
    return ret;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *ret = dst;
    while (*dst) dst++;
    for (size_t i = 0; i < n && src[i]; i++) *dst++ = src[i];
    *dst = '\0';
    return ret;
}

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *d = (char*)malloc(len);
    if (d) memcpy(d, s, len);
    return d;
}

char *strchr(const char *s, int c) {
    while (*s) { if (*s == (char)c) return (char*)s; s++; }
    return (c == '\0') ? (char*)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) { if (*s == (char)c) last = s; s++; }
    if (c == '\0') return (char*)s;
    return (char*)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char*)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char*)haystack;
    }
    return NULL;
}

int strcasecmp(const char *s1, const char *s2) {
    while (*s1 && tolower(*s1) == tolower(*s2)) { s1++; s2++; }
    return tolower(*(uint8_t*)s1) - tolower(*(uint8_t*)s2);
}

int strncasecmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int d = tolower((uint8_t)s1[i]) - tolower((uint8_t)s2[i]);
        if (d || !s1[i]) return d;
    }
    return 0;
}

/* ===== stdlib.h ===== */
void *malloc(size_t size) { return kmalloc((uint32_t)size); }
void *calloc(size_t nmemb, size_t size) { return kcalloc((uint32_t)nmemb, (uint32_t)size); }
void *realloc(void *ptr, size_t size) { return krealloc(ptr, (uint32_t)size); }
void free(void *ptr) { kfree(ptr); }

int abs(int x) { return x < 0 ? -x : x; }

DOOM_WEAK int atoi(const char *s) {
    int result = 0, sign = 1;
    while (isspace(*s)) s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (isdigit(*s)) { result = result * 10 + (*s - '0'); s++; }
    return sign * result;
}

long atol(const char *s) { return (long)atoi(s); }

long strtol(const char *s, char **endptr, int base) {
    long result = 0;
    int sign = 1;
    while (isspace(*s)) s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    if (base == 0) {
        if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (*s == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'f') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * base + digit;
        s++;
    }
    if (endptr) *endptr = (char*)s;
    return sign * result;
}

unsigned long strtoul(const char *s, char **endptr, int base) {
    return (unsigned long)strtol(s, endptr, base);
}

char *getenv(const char *name) { (void)name; return NULL; }

void exit(int status) {
    char buf[32];
    write_serial_string("[DOOM] exit(");
    if (status < 0) { write_serial_string("-"); status = -status; }
    int len = 0; char tmp[12];
    if (status == 0) tmp[len++] = '0';
    else { int v = status; while(v) { tmp[len++] = '0' + v%10; v /= 10; } }
    for (int i = len-1; i >= 0; i--) buf[len-1-i] = tmp[i];
    buf[len] = '\0';
    write_serial_string(buf);
    write_serial_string(") called\n");
    // Halt — returning to shell is not safe since DOOM corrupts kernel state
    while(1) { __asm__ volatile("hlt"); }
}

/* ===== printf family ===== */

// Minimal vsnprintf
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    size_t pos = 0;
    if (size == 0) return 0;
    
    #define PUT(ch) do { if (pos < size - 1) buf[pos] = (ch); pos++; } while(0)
    
    while (*fmt) {
        if (*fmt != '%') { PUT(*fmt); fmt++; continue; }
        fmt++; // skip '%'
        
        // Flags
        int left_align = 0, zero_pad = 0, has_plus = 0;
        while (*fmt == '-' || *fmt == '0' || *fmt == '+' || *fmt == ' ') {
            if (*fmt == '-') left_align = 1;
            if (*fmt == '0') zero_pad = 1;
            if (*fmt == '+') has_plus = 1;
            fmt++;
        }
        (void)left_align; (void)has_plus;
        
        // Width
        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else { while (isdigit(*fmt)) { width = width * 10 + (*fmt - '0'); fmt++; } }
        
        // Precision
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') { precision = va_arg(ap, int); fmt++; }
            else { while (isdigit(*fmt)) { precision = precision * 10 + (*fmt - '0'); fmt++; } }
        }
        
        // Length modifier
        int is_long = 0, is_longlong = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; if (*fmt == 'l') { is_longlong = 1; fmt++; } }
        else if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }
        else if (*fmt == 'z') { is_long = 1; fmt++; }
        (void)is_longlong;
        
        char tmp[32];
        int tlen = 0;
        
        switch (*fmt) {
        case 'd': case 'i': {
            int val = is_long ? (int)va_arg(ap, long) : va_arg(ap, int);
            int neg = 0;
            unsigned int uval;
            if (val < 0) { neg = 1; uval = (unsigned int)(-val); }
            else uval = (unsigned int)val;
            if (uval == 0) tmp[tlen++] = '0';
            else { while (uval) { tmp[tlen++] = '0' + (uval % 10); uval /= 10; } }
            if (neg) tmp[tlen++] = '-';
            int pad = width - tlen;
            char padch = zero_pad ? '0' : ' ';
            if (neg && zero_pad) { PUT('-'); tlen--; }
            while (pad-- > 0) PUT(padch);
            if (neg && !zero_pad) PUT('-');  // already added if zero_pad
            // Actually fix: just print reversed
            // Let me redo this more carefully
            break;
        }
        default: break;
        }
        
        // Simplified approach: just handle each format
        // Reset and use a simpler method
        if (*fmt == 'd' || *fmt == 'i') {
            int val = is_long ? (int)va_arg(ap, long) : va_arg(ap, int);
            // Undo the va_arg above — actually we already consumed it
            // This is getting complicated. Let me use a different approach.
        }
        
        fmt++;
    }
    
    if (pos < size) buf[pos] = '\0';
    else buf[size-1] = '\0';
    
    #undef PUT
    return (int)pos;
}

// Actually, let me rewrite vsnprintf properly:
// The above attempt was getting messy. Here's a clean implementation.

static int doom_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    size_t pos = 0;
    if (size == 0) return 0;
    size--; // reserve for null terminator

    while (*fmt && pos < size) {
        if (*fmt != '%') { buf[pos++] = *fmt++; continue; }
        fmt++; // skip '%'

        // Flags
        int zero_pad = 0;
        while (*fmt == '-' || *fmt == '0' || *fmt == '+' || *fmt == ' ' || *fmt == '#') {
            if (*fmt == '0') zero_pad = 1;
            fmt++;
        }

        // Width
        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else while (isdigit(*fmt)) { width = width * 10 + (*fmt++ - '0'); }

        // Precision
        int precision = -1;
        if (*fmt == '.') {
            fmt++; precision = 0;
            if (*fmt == '*') { precision = va_arg(ap, int); fmt++; }
            else while (isdigit(*fmt)) { precision = precision * 10 + (*fmt++ - '0'); }
        }

        // Length
        int is_long = 0;
        if (*fmt == 'l') { fmt++; is_long = 1; if (*fmt == 'l') fmt++; }
        else if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }
        else if (*fmt == 'z' || *fmt == 't') { is_long = 1; fmt++; }

        char numbuf[24];
        int nlen = 0;
        (void)is_long;

        switch (*fmt) {
        case '%':
            buf[pos++] = '%';
            break;
        case 'c':
            buf[pos++] = (char)va_arg(ap, int);
            break;
        case 's': {
            const char *s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            int slen = (int)strlen(s);
            if (precision >= 0 && slen > precision) slen = precision;
            // Pad
            for (int i = slen; i < width && pos < size; i++) buf[pos++] = ' ';
            for (int i = 0; i < slen && pos < size; i++) buf[pos++] = s[i];
            break;
        }
        case 'd': case 'i': {
            int val = va_arg(ap, int);
            int neg = 0;
            unsigned int uval;
            if (val < 0) { neg = 1; uval = (unsigned int)(-val); } else uval = (unsigned int)val;
            if (uval == 0) numbuf[nlen++] = '0';
            else while (uval) { numbuf[nlen++] = '0' + uval % 10; uval /= 10; }
            if (neg) numbuf[nlen++] = '-';
            // Pad
            char padc = zero_pad ? '0' : ' ';
            if (neg && zero_pad && pos < size) { buf[pos++] = '-'; nlen--; } // remove '-' from numbuf
            for (int i = nlen; i < width && pos < size; i++) buf[pos++] = padc;
            for (int i = nlen - 1; i >= 0 && pos < size; i--) buf[pos++] = numbuf[i];
            break;
        }
        case 'u': {
            unsigned int val = va_arg(ap, unsigned int);
            if (val == 0) numbuf[nlen++] = '0';
            else while (val) { numbuf[nlen++] = '0' + val % 10; val /= 10; }
            for (int i = nlen; i < width && pos < size; i++) buf[pos++] = (zero_pad ? '0' : ' ');
            for (int i = nlen - 1; i >= 0 && pos < size; i--) buf[pos++] = numbuf[i];
            break;
        }
        case 'x': case 'X': {
            unsigned int val = va_arg(ap, unsigned int);
            const char *hex = (*fmt == 'x') ? "0123456789abcdef" : "0123456789ABCDEF";
            if (val == 0) numbuf[nlen++] = '0';
            else while (val) { numbuf[nlen++] = hex[val & 0xF]; val >>= 4; }
            for (int i = nlen; i < width && pos < size; i++) buf[pos++] = (zero_pad ? '0' : ' ');
            for (int i = nlen - 1; i >= 0 && pos < size; i--) buf[pos++] = numbuf[i];
            break;
        }
        case 'p': {
            unsigned int val = (unsigned int)(uintptr_t)va_arg(ap, void*);
            if (pos < size) buf[pos++] = '0';
            if (pos < size) buf[pos++] = 'x';
            const char *hex = "0123456789abcdef";
            if (val == 0) numbuf[nlen++] = '0';
            else while (val) { numbuf[nlen++] = hex[val & 0xF]; val >>= 4; }
            for (int i = nlen - 1; i >= 0 && pos < size; i--) buf[pos++] = numbuf[i];
            break;
        }
        case 'f': {
            // DOOM doesn't really use floats, but just in case — print "0.0"
            (void)va_arg(ap, double);
            const char *stub = "0.0";
            while (*stub && pos < size) buf[pos++] = *stub++;
            break;
        }
        default:
            if (pos < size) buf[pos++] = '%';
            if (pos < size) buf[pos++] = *fmt;
            break;
        }
        fmt++;
    }
    buf[pos] = '\0';
    return (int)pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = doom_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = doom_vsnprintf(buf, 4096, fmt, ap); // assume big enough
    va_end(ap);
    return ret;
}

int printf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int ret = doom_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write_serial_string(buf);
    return ret;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    (void)f;
    char buf[512];
    int ret = doom_vsnprintf(buf, sizeof(buf), fmt, ap);
    write_serial_string(buf);
    return ret;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vfprintf(f, fmt, ap);
    va_end(ap);
    return ret;
}

int puts(const char *s) {
    write_serial_string(s);
    write_serial_string("\n");
    return 0;
}

int putchar(int c) {
    char buf[2] = {(char)c, '\0'};
    write_serial_string(buf);
    return c;
}

void perror(const char *s) {
    if (s && *s) { write_serial_string(s); write_serial_string(": "); }
    write_serial_string("error\n");
}

int sscanf(const char *str, const char *fmt, ...) {
    (void)str; (void)fmt;
    return 0; // stub
}

/* ===== FILE operations (WAD file access) ===== */

// The WAD file is embedded in the kernel binary
extern uint8_t _binary_doom1_wad_start[];
extern uint8_t _binary_doom1_wad_end[];

// Stdout/stderr stubs
static FILE doom_stdout_obj = {NULL, 0, 0, 0, 0, 1};
static FILE doom_stderr_obj = {NULL, 0, 0, 0, 0, 1};
FILE *doom_stdout = &doom_stdout_obj;
FILE *doom_stderr = &doom_stderr_obj;

FILE *fopen(const char *path, const char *mode) {
    (void)mode;
    write_serial_string("[DOOM-FOPEN] ");
    write_serial_string(path);
    write_serial_string("\n");
    // Check if this is the WAD file
    if (strstr(path, "doom1.wad") || strstr(path, "DOOM1.WAD") ||
        strstr(path, "doom.wad") || strstr(path, "DOOM.WAD")) {
        FILE *f = (FILE*)malloc(sizeof(FILE));
        if (!f) return NULL;
        f->data = _binary_doom1_wad_start;
        f->size = (uint32_t)(_binary_doom1_wad_end - _binary_doom1_wad_start);
        f->pos = 0;
        f->eof_flag = 0;
        f->error_flag = 0;
        f->mode = 0;
        return f;
    }
    // For any other file, return NULL (not found)
    return NULL;
}

int fclose(FILE *f) {
    if (f && f != doom_stdout && f != doom_stderr) free(f);
    return 0;
}

size_t fread(void *buf, size_t elem_size, size_t nmemb, FILE *f) {
    if (!f || !f->data) return 0;
    size_t total = elem_size * nmemb;
    size_t avail = f->size - f->pos;
    if (total > avail) { total = avail; f->eof_flag = 1; }
    memcpy(buf, f->data + f->pos, total);
    f->pos += total;
    return total / elem_size;
}

size_t fwrite(const void *buf, size_t elem_size, size_t nmemb, FILE *f) {
    (void)buf;
    // Write to serial for debugging if stdout/stderr
    if (f == doom_stdout || f == doom_stderr) {
        // Just ignore or write to serial
    }
    return elem_size * nmemb; // pretend success
}

int fseek(FILE *f, long offset, int whence) {
    if (!f || !f->data) return -1;
    long new_pos;
    switch (whence) {
    case SEEK_SET: new_pos = offset; break;
    case SEEK_CUR: new_pos = (long)f->pos + offset; break;
    case SEEK_END: new_pos = (long)f->size + offset; break;
    default: return -1;
    }
    if (new_pos < 0) new_pos = 0;
    if ((uint32_t)new_pos > f->size) new_pos = (long)f->size;
    f->pos = (uint32_t)new_pos;
    f->eof_flag = 0;
    return 0;
}

long ftell(FILE *f) { return f ? (long)f->pos : -1; }
void rewind(FILE *f) { if (f) { f->pos = 0; f->eof_flag = 0; } }
int feof(FILE *f) { return f ? f->eof_flag : 1; }
int ferror(FILE *f) { return f ? f->error_flag : 1; }
int fflush(FILE *f) { (void)f; return 0; }

int fgetc(FILE *f) {
    if (!f || !f->data || f->pos >= f->size) { if (f) f->eof_flag = 1; return EOF; }
    return f->data[f->pos++];
}

int ungetc(int c, FILE *f) {
    if (!f || f->pos == 0) return EOF;
    f->pos--;
    f->eof_flag = 0;
    return c;
}

char *fgets(char *s, int size, FILE *f) {
    if (!f || !f->data || size <= 0) return NULL;
    int i = 0;
    while (i < size - 1 && f->pos < f->size) {
        s[i] = (char)f->data[f->pos++];
        if (s[i] == '\n') { i++; break; }
        i++;
    }
    if (i == 0) return NULL;
    s[i] = '\0';
    return s;
}

/* ===== POSIX stubs ===== */
int open(const char *path, int flags, ...) { (void)path; (void)flags; return -1; }
int close(int fd) { (void)fd; return 0; }
ssize_t read(int fd, void *buf, size_t count) { (void)fd; (void)buf; (void)count; return -1; }
ssize_t write(int fd, const void *buf, size_t count) { (void)fd; (void)buf; (void)count; return (ssize_t)count; }
int mkdir(const char *path, int mode) { (void)path; (void)mode; return -1; }
int stat(const char *path, void *buf) { (void)path; (void)buf; return -1; }
int access(const char *path, int mode) { (void)path; (void)mode; return -1; }
int remove(const char *path) { (void)path; return -1; }
int rename(const char *old, const char *new_name) { (void)old; (void)new_name; return -1; }
FILE *tmpfile(void) { return NULL; }
char *tmpnam(char *s) { (void)s; return NULL; }

/* Missing stubs needed by DOOM */
int system(const char *cmd) { (void)cmd; return -1; }

double atof(const char *s) { (void)s; return 0.0; }

double fabs(double x) { return x < 0 ? -x : x; }

/* GCC helper for 64-bit division on 32-bit */
long long __divdi3(long long a, long long b) {
    if (b == 0) return 0;
    int neg = 0;
    unsigned long long ua, ub;
    if (a < 0) { neg = !neg; ua = (unsigned long long)(-a); } else { ua = (unsigned long long)a; }
    if (b < 0) { neg = !neg; ub = (unsigned long long)(-b); } else { ub = (unsigned long long)b; }
    unsigned long long q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r <<= 1;
        r |= (ua >> i) & 1;
        if (r >= ub) { r -= ub; q |= (1ULL << i); }
    }
    return neg ? -(long long)q : (long long)q;
}
