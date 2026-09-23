/* Umbrella for the small freestanding headers the ioquake3 subset needs.
 * NOTE: guards use the Q3PARTS_ prefix — the individual stub headers
 * (stddef.h, time.h, errno.h, ...) have their own Q3STUB_* guards and are
 * the canonical source for those names; this file only covers the ones
 * included under their own filename.
 */

/* ---- stddef.h ---- */
#ifndef Q3STUB_STDDEF_H
#define Q3STUB_STDDEF_H
#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef unsigned int size_t;
#endif
typedef int ptrdiff_t;
#define NULL ((void*)0)
#define offsetof(t, m) __builtin_offsetof(t, m)
#endif

/* ---- stdint.h ---- */
#ifndef Q3STUB_STDINT_H
#define Q3STUB_STDINT_H
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned int       uintptr_t;
typedef int                intptr_t;
typedef int8_t   int_least8_t;
typedef int16_t  int_least16_t;
typedef int32_t  int_least32_t;
typedef int64_t  int_least64_t;
typedef uint8_t  uint_least8_t;
typedef uint16_t uint_least16_t;
typedef uint32_t uint_least32_t;
typedef uint64_t uint_least64_t;
typedef int8_t   int_fast8_t;
typedef int32_t  int_fast16_t;
typedef int32_t  int_fast32_t;
typedef int64_t  int_fast64_t;
typedef uint8_t  uint_fast8_t;
typedef uint32_t uint_fast16_t;
typedef uint32_t uint_fast32_t;
typedef uint64_t uint_fast64_t;
typedef long     intmax_t;
typedef unsigned long uintmax_t;
#define INT8_MIN   (-128)
#define INT16_MIN  (-32768)
#define INT32_MIN  (-2147483647-1)
#define INT8_MAX   127
#define INT16_MAX  32767
#define INT32_MAX  2147483647
#define UINT8_MAX  255
#define UINT16_MAX 65535
#define UINT32_MAX 4294967295u
#define INT64_MAX  9223372036854775807LL
#define UINT64_MAX 18446744073709551615ULL
#define INTPTR_MIN INT32_MIN
#define INTPTR_MAX INT32_MAX
#define UINTPTR_MAX UINT32_MAX
#define SIZE_MAX   UINT32_MAX
#endif

/* ---- stdbool.h ---- */
#ifndef Q3STUB_STDBOOL_H
#define Q3STUB_STDBOOL_H
#define bool  _Bool
#define true  1
#define false 0
#define __bool_true_false_are_defined 1
#endif

/* ---- limits.h ---- */
#ifndef Q3STUB_LIMITS_H
#define Q3STUB_LIMITS_H
#define CHAR_BIT  8
#define SCHAR_MIN (-128)
#define SCHAR_MAX 127
#define UCHAR_MAX 255
#define CHAR_MIN  SCHAR_MIN
#define CHAR_MAX  SCHAR_MAX
#define SHRT_MIN  (-32768)
#define SHRT_MAX  32767
#define USHRT_MAX 65535
#define INT_MIN   (-2147483647-1)
#define INT_MAX   2147483647
#define UINT_MAX  4294967295u
#define LONG_MIN  (-2147483647L-1)
#define LONG_MAX  2147483647L
#define ULONG_MAX 4294967295UL
#define LLONG_MIN (-9223372036854775807LL-1)
#define LLONG_MAX 9223372036854775807LL
#define ULLONG_MAX 18446744073709551615ULL
#endif

/* ---- float.h ---- */
#ifndef Q3STUB_FLOAT_H
#define Q3STUB_FLOAT_H
#define FLT_RADIX       2
#define FLT_MANT_DIG    24
#define DBL_MANT_DIG    53
#define FLT_DIG         6
#define DBL_DIG         15
#define FLT_MIN_EXP     -125
#define DBL_MIN_EXP     -1021
#define FLT_MAX_EXP     128
#define DBL_MAX_EXP     1024
#define FLT_MAX         3.40282346638528859812e+38F
#define FLT_MIN         1.17549435082228750797e-38F
#define FLT_EPSILON     1.19209289550781250000e-7F
#define DBL_MAX         1.79769313486231570815e+308
#define DBL_MIN         2.22507385850720138309e-308
#define DBL_EPSILON     2.22044604925031308085e-16
#endif
