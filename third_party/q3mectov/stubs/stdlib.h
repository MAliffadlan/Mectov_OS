/* <stdlib.h> stub — ioquake3 subset on Mectov OS kernel (v38.98).
 * All names aliased to q3_* implementations in q3_kernel.c.
 */
#ifndef Q3STUB_STDLIB_H
#define Q3STUB_STDLIB_H

#include "stddef.h"

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647

void *q3_malloc(size_t size);
void *q3_calloc(size_t num, size_t size);
void *q3_realloc(void *ptr, size_t new_size);
void  q3_free(void *ptr);
void  q3_exit(int status) __attribute__((noreturn));
void  q3_abort(void) __attribute__((noreturn));
int   q3_atoi(const char *s);
long  q3_atol(const char *s);
double q3_atof(const char *s);
long  q3_strtol(const char *s, char **endp, int base);
unsigned long q3_strtoul(const char *s, char **endp, int base);
int   q3_rand(void);
void  q3_srand(unsigned int seed);
char *q3_getenv(const char *name);
int   q3_system(const char *cmd);
void  q3_qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *));
int   q3_abs(int x);
long  q3_labs(long x);

#define malloc   q3_malloc
#define calloc   q3_calloc
#define realloc  q3_realloc
#define free     q3_free
#define exit     q3_exit
#define abort    q3_abort
#define atoi     q3_atoi
#define atol     q3_atol
#define atof     q3_atof
#define strtol   q3_strtol
#define strtoul  q3_strtoul
#define rand     q3_rand
#define srand    q3_srand
#define getenv   q3_getenv
#define system   q3_system
#define qsort    q3_qsort
#define abs      q3_abs
#define labs     q3_labs

#endif /* Q3STUB_STDLIB_H */
