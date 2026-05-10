#ifndef LIBC_H
#define LIBC_H

#include "../../src/include/syscall.h"

// --- Mectov OS Libc Export Table Pointer ---
extern void** __mct_lib_ptr;

// --- Dynamic Library Loader ---
static inline void* mct_load_library(const char* lib_name) {
    return (void*)syscall(SYS_LOAD_LIBRARY, (int)lib_name, 0, 0);
}

// --- Standard Libc Functions (Dynamic) ---

// These functions will be called via the loaded export table.
// Order MUST match the export table in libc.c

#define LIB_FUNC_STRLEN   0
#define LIB_FUNC_STRCPY   1
#define LIB_FUNC_STRCAT   2
#define LIB_FUNC_ITOA     3
#define LIB_FUNC_ITOA_PAD 4
#define LIB_FUNC_ATOI     5
#define LIB_FUNC_RAND     6

static inline int my_strlen(const char* s) {
    int (*fn)(const char*) = (int (*)(const char*))__mct_lib_ptr[LIB_FUNC_STRLEN];
    return fn(s);
}

static inline char* my_strcpy(char* dest, const char* src) {
    char* (*fn)(char*, const char*) = (char* (*)(char*, const char*))__mct_lib_ptr[LIB_FUNC_STRCPY];
    return fn(dest, src);
}

static inline char* my_strcat(char* dest, const char* src) {
    char* (*fn)(char*, const char*) = (char* (*)(char*, const char*))__mct_lib_ptr[LIB_FUNC_STRCAT];
    return fn(dest, src);
}

static inline void my_itoa(int n, char* buf) {
    void (*fn)(int, char*) = (void (*)(int, char*))__mct_lib_ptr[LIB_FUNC_ITOA];
    fn(n, buf);
}

static inline void itoa_pad(int n, char* buf, int pad) {
    void (*fn)(int, char*, int) = (void (*)(int, char*, int))__mct_lib_ptr[LIB_FUNC_ITOA_PAD];
    fn(n, buf, pad);
}

static inline int my_atoi(const char* s) {
    int (*fn)(const char*) = (int (*)(const char*))__mct_lib_ptr[LIB_FUNC_ATOI];
    return fn(s);
}

static inline int my_rand() {
    int (*fn)() = (int (*)())__mct_lib_ptr[LIB_FUNC_RAND];
    return fn();
}

#endif
