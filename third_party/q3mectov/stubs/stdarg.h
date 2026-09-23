/* <stdarg.h> — compiler builtins (freestanding, no host libc) */
#ifndef Q3STUB_STDARG_H
#define Q3STUB_STDARG_H
typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v)      __builtin_va_end(v)
#define va_arg(v, t)   __builtin_va_arg(v, t)
#define va_copy(d, s)  __builtin_va_copy(d, s)
#endif
