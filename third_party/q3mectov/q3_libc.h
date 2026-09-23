/* q3_libc.h — libc shim for the ioquake3 subset on Mectov OS.
 * Host spike: forwards to real glibc. Kernel port: same names resolve to
 * the shim implementations in q3_libc.c (doom_libc.c pattern).
 */
#ifndef Q3_LIBC_H
#define Q3_LIBC_H

#ifdef Q3_HOST_SPIKE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#else
/* Kernel build: real declarations, shim provides definitions. */
#include <stddef.h>
#include <stdarg.h>
typedef long ssize_t_like;  /* not used by ioq3 subset */
#endif

#endif /* Q3_LIBC_H */
