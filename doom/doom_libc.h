/*
 * doom_libc.h - Mini C Standard Library for DOOM on Mectov OS
 * Provides all standard library headers that DOOM expects.
 */
#ifndef DOOM_LIBC_H
#define DOOM_LIBC_H

/* ===== stdint.h ===== */
#ifndef _STDINT_H
#define _STDINT_H
typedef unsigned char      uint8_t;
typedef signed char        int8_t;
typedef unsigned short     uint16_t;
typedef signed short       int16_t;
typedef unsigned int       uint32_t;
typedef signed int         int32_t;
typedef unsigned long long uint64_t;
typedef signed long long   int64_t;
typedef uint32_t           uintptr_t;
typedef int32_t            intptr_t;
typedef uint32_t           size_t;
typedef int32_t            ssize_t;
typedef int32_t            ptrdiff_t;

#define UINT32_MAX 0xFFFFFFFFU
#define INT32_MAX  0x7FFFFFFF
#define INT32_MIN  (-INT32_MAX - 1)
#define UINT16_MAX 0xFFFF
#define INT16_MAX  0x7FFF
#define INT16_MIN  (-INT16_MAX - 1)
#define UINT8_MAX  0xFF
#define INT8_MAX   0x7F
#define INT8_MIN   (-INT8_MAX - 1)
#define SIZE_MAX   UINT32_MAX
#endif

/* ===== stdbool.h ===== */
#ifndef _STDBOOL_H
#define _STDBOOL_H
#define bool  _Bool
#define true  1
#define false 0
#define __bool_true_false_are_defined 1
#endif

/* ===== stddef.h ===== */
#ifndef NULL
#define NULL ((void*)0)
#endif
#define offsetof(type, member) ((size_t)&(((type *)0)->member))

/* ===== limits.h ===== */
#define CHAR_BIT 8
#define INT_MAX  INT32_MAX
#define INT_MIN  INT32_MIN
#define UINT_MAX UINT32_MAX
#define LONG_MAX INT32_MAX
#define LONG_MIN INT32_MIN
#define ULONG_MAX UINT32_MAX
#define PATH_MAX 256
#define SCHAR_MAX 127
#define SCHAR_MIN (-128)
#define UCHAR_MAX 255
#define SHRT_MAX  INT16_MAX
#define SHRT_MIN  INT16_MIN
#define USHRT_MAX UINT16_MAX

/* ===== stdarg.h ===== */
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_copy(d, s)      __builtin_va_copy(d, s)

/* ===== errno.h ===== */
extern int doom_errno;
#define errno doom_errno
#define ENOENT 2
#define ENOMEM 12
#define EINVAL 22
#define EISDIR 21

/* ===== assert.h ===== */
#define assert(x) ((void)0)

/* ===== ctype.h ===== */
int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isspace(int c);
int isprint(int c);
int isupper(int c);
int islower(int c);
int toupper(int c);
int tolower(int c);

/* ===== string.h ===== */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int   memcmp(const void *s1, const void *s2, size_t n);
size_t strlen(const char *s);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
int   strcmp(const char *s1, const char *s2);
int   strncmp(const char *s1, const char *s2, size_t n);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t n);
char *strdup(const char *s);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
int   strcasecmp(const char *s1, const char *s2);
int   strncasecmp(const char *s1, const char *s2, size_t n);

/* ===== stdlib.h ===== */
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);
int   atoi(const char *s);
long  atol(const char *s);
long  strtol(const char *s, char **endptr, int base);
unsigned long strtoul(const char *s, char **endptr, int base);
int   abs(int x);
void  exit(int status);
int   rand(void);
void  srand(unsigned int s);
#define RAND_MAX 0x7FFFFFFF
char *getenv(const char *name);

/* ===== stdio.h ===== */
typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint32_t pos;
    int      eof_flag;
    int      error_flag;
    int      mode; // 0=read, 1=write (stubbed)
} FILE;

extern FILE *doom_stdout;
extern FILE *doom_stderr;
#define stdout doom_stdout
#define stderr doom_stderr
#define stdin  ((FILE*)NULL)

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

FILE *fopen(const char *path, const char *mode);
int   fclose(FILE *f);
size_t fread(void *buf, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t nmemb, FILE *f);
int   fseek(FILE *f, long offset, int whence);
long  ftell(FILE *f);
void  rewind(FILE *f);
int   feof(FILE *f);
int   ferror(FILE *f);
int   fflush(FILE *f);
int   fgetc(FILE *f);
int   ungetc(int c, FILE *f);
char *fgets(char *s, int size, FILE *f);
int   fprintf(FILE *f, const char *fmt, ...);
int   printf(const char *fmt, ...);
int   sprintf(char *buf, const char *fmt, ...);
int   snprintf(char *buf, size_t size, const char *fmt, ...);
int   vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int   vfprintf(FILE *f, const char *fmt, va_list ap);
int   sscanf(const char *str, const char *fmt, ...);
int   puts(const char *s);
int   putchar(int c);
void  perror(const char *s);
int   remove(const char *path);
int   rename(const char *old, const char *new_name);
FILE *tmpfile(void);
char *tmpnam(char *s);

/* ===== inttypes.h ===== */
#define PRIi64 "lld"
#define PRIu64 "llu"
#define PRIx64 "llx"
#define PRId32 "d"
#define PRIu32 "u"
#define PRIx32 "x"

/* ===== fcntl.h / unistd.h stubs ===== */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200

int   open(const char *path, int flags, ...);
int   close(int fd);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int   mkdir(const char *path, int mode);
int   stat(const char *path, void *buf);
int   access(const char *path, int mode);
#define F_OK 0
#define R_OK 4
#define W_OK 2

/* ===== sys/stat.h ===== */
struct stat {
    uint32_t st_size;
    uint32_t st_mode;
};
#define S_ISREG(m) (1)
#define S_ISDIR(m) (0)

/* ===== sys/types.h ===== */
/* Already defined above */

/* ===== math.h (minimal) ===== */
#define M_PI 3.14159265358979323846
/* DOOM doesn't use floating point heavily, but just in case */

/* ===== strings.h ===== */
/* strcasecmp/strncasecmp already declared above */

#endif /* DOOM_LIBC_H */
