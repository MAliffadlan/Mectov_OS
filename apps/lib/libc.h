#ifndef LIBC_H
#define LIBC_H

#include "../../src/include/syscall.h"

// --- Mectov OS Libc Export Table Pointer ---
extern void** __mct_lib_ptr;

// --- Dynamic Library Loader ---
static inline void** mct_load_library(const char* lib_name) {
    char abs_path[64];
    const char* real_path = lib_name;
    if (lib_name && lib_name[0] != '/') {
        int len = 0;
        while (lib_name[len]) len++;
        if (len < 60) {
            abs_path[0] = '/';
            int i = 0;
            while (lib_name[i]) {
                abs_path[i + 1] = lib_name[i];
                i++;
            }
            abs_path[i + 1] = '\0';
            real_path = abs_path;
        }
    }
    uint32_t base = (uint32_t)syscall(SYS_LOAD_LIBRARY, (int)real_path, 0, 0);
    if (base == 0 || base == 0xFFFFFFFF) return 0;
    // The export table starts after the 8-byte header (Magic + Count)
    return (void**)(base + 8);
}

// --- Standard POSIX / C Library Functions (Dynamic) ---

#define LIB_FUNC_STRLEN   0
#define LIB_FUNC_STRCPY   1
#define LIB_FUNC_STRCAT   2
#define LIB_FUNC_ITOA     3
#define LIB_FUNC_ITOA_PAD 4
#define LIB_FUNC_ATOI     5
#define LIB_FUNC_RAND     6
#define LIB_FUNC_PRINTF   7
#define LIB_FUNC_SPRINTF  8

// --- String & Math ---
static inline int strlen(const char* s) {
    int (*fn)(const char*) = (int (*)(const char*))__mct_lib_ptr[LIB_FUNC_STRLEN];
    return fn(s);
}

static inline char* strcpy(char* dest, const char* src) {
    char* (*fn)(char*, const char*) = (char* (*)(char*, const char*))__mct_lib_ptr[LIB_FUNC_STRCPY];
    return fn(dest, src);
}

static inline char* strcat(char* dest, const char* src) {
    char* (*fn)(char*, const char*) = (char* (*)(char*, const char*))__mct_lib_ptr[LIB_FUNC_STRCAT];
    return fn(dest, src);
}

static inline void itoa(int n, char* buf) {
    void (*fn)(int, char*) = (void (*)(int, char*))__mct_lib_ptr[LIB_FUNC_ITOA];
    fn(n, buf);
}

static inline void itoa_pad(int n, char* buf, int pad) {
    void (*fn)(int, char*, int) = (void (*)(int, char*, int))__mct_lib_ptr[LIB_FUNC_ITOA_PAD];
    fn(n, buf, pad);
}

static inline int atoi(const char* s) {
    int (*fn)(const char*) = (int (*)(const char*))__mct_lib_ptr[LIB_FUNC_ATOI];
    return fn(s);
}

static inline int rand() {
    int (*fn)() = (int (*)())__mct_lib_ptr[LIB_FUNC_RAND];
    return fn();
}

#ifndef BUILDING_LIBC
static void __attribute__((noinline)) printf(const char* format, ...) {
    void (*fn)(const char*, void*) = (void (*)(const char*, void*))__mct_lib_ptr[LIB_FUNC_PRINTF];
    fn(format, (void*)(&format + 1));
}

static void __attribute__((noinline)) sprintf(char* buf, const char* format, ...) {
    void (*fn)(char*, const char*, void*) = (void (*)(char*, const char*, void*))__mct_lib_ptr[LIB_FUNC_SPRINTF];
    fn(buf, format, (void*)(&format + 1));
}
#endif


// --- POSIX System Call Wrappers ---

static inline int open(const char* pathname, int flags) {
    (void)flags; // Currently Mectov ignores flags
    return sys_open(pathname);
}

static inline int read(int fd, void* buf, int count) {
    return sys_read(fd, (char*)buf, count);
}

static inline int write(int fd, const void* buf, int count) {
    return sys_write(fd, (const char*)buf, count);
}

static inline int close(int fd) {
    sys_close(fd);
    return 0;
}

static inline void* malloc(int size) {
    return sys_malloc(size);
}

static inline void free(void* ptr) {
    sys_free(ptr);
}

static inline void exit(int status) {
    syscall(SYS_EXIT, status, 0, 0);
    for(;;);
}

// --- Process model (fork / waitpid / signals) ---
static inline int fork(void) {
    return sys_fork();
}

static inline int waitpid(int pid, int* status, int options) {
    return sys_waitpid(pid, status, options);
}

static inline int kill(int pid, int sig) {
    return sys_kill(pid, sig);
}

static inline int getppid(void) {
    return sys_getppid();
}

// handler: 0 = default, 1 = SIG_IGN, otherwise a function pointer
static inline void* signal(int sig, void* handler) {
    return sys_signal(sig, handler);
}

static inline void sleep(int seconds) {
    // PIT runs at 1000Hz, so get_ticks() counts milliseconds.
    uint32_t start = sys_get_ticks();
    while (sys_get_ticks() < start + (uint32_t)(seconds * 1000)) {
        sys_sleep(100); // sleep in 100ms slices so signals stay deliverable
    }
}

#endif
