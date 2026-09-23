/* <stdio.h> stub — ioquake3 subset on Mectov OS kernel (v38.98).
 * Resolved via -Ithird_party/q3mectov/stubs. Every libc name is #define-aliased
 * to a q3_* symbol implemented in q3_kernel.c, so ioq3 object code never
 * references (or collides with) the kernel's own libc / doom_libc symbols.
 * FILE streams are backed by the Mectov VFS.
 */
#ifndef Q3STUB_STDIO_H
#define Q3STUB_STDIO_H

#include "stddef.h"
#include "stdarg.h"

typedef struct q3_FILE FILE;

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define EOF (-1)

extern FILE *q3_stdout;
extern FILE *q3_stderr;
#define stdout q3_stdout
#define stderr q3_stderr

FILE *q3_fopen(const char *path, const char *mode);
int   q3_fclose(FILE *f);
size_t q3_fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t q3_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int   q3_fseek(FILE *f, long off, int whence);
long  q3_ftell(FILE *f);
int   q3_feof(FILE *f);
int   q3_ferror(FILE *f);
int   q3_fflush(FILE *f);
char *q3_fgets(char *buf, int size, FILE *f);
int   q3_ungetc(int c, FILE *f);
int   q3_remove(const char *path);
int   q3_rename(const char *old, const char *new);
int   q3_fputs(const char *s, FILE *f);
int   q3_fputc(int c, FILE *f);
int   q3_fgetc(FILE *f);
void  q3_rewind(FILE *f);
int   q3_fprintf(FILE *f, const char *fmt, ...);
int   q3_vfprintf(FILE *f, const char *fmt, va_list ap);
int   q3_printf(const char *fmt, ...);
int   q3_vprintf(const char *fmt, va_list ap);

int q3_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int q3_snprintf(char *buf, size_t size, const char *fmt, ...);
int q3_sprintf(char *buf, const char *fmt, ...);

#define fopen     q3_fopen
#define fclose    q3_fclose
#define fread     q3_fread
#define fwrite    q3_fwrite
#define fseek     q3_fseek
#define ftell     q3_ftell
#define feof      q3_feof
#define ferror    q3_ferror
#define fflush    q3_fflush
#define fgets     q3_fgets
#define ungetc    q3_ungetc
#define remove    q3_remove
#define rename    q3_rename
#define fputs     q3_fputs
#define fputc     q3_fputc
#define fgetc     q3_fgetc
#define rewind    q3_rewind
#define fprintf   q3_fprintf
#define vfprintf  q3_vfprintf
#define printf    q3_printf
#define vprintf   q3_vprintf
#define sprintf   q3_sprintf
#define snprintf  q3_snprintf
#define vsnprintf q3_vsnprintf

#define _IONBF 2
#define _IOFBF 0
static inline int q3_setvbuf(FILE *f, char *b, int m, size_t sz) { (void)f; (void)b; (void)m; (void)sz; return 0; }
#define setvbuf q3_setvbuf

#endif /* Q3STUB_STDIO_H */
