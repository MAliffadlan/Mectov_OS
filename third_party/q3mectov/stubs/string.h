/* <string.h> stub — ioquake3 subset on Mectov OS kernel (v38.98).
 * Names aliased to q3_* implementations in q3_kernel.c.
 */
#ifndef Q3STUB_STRING_H
#define Q3STUB_STRING_H

#include "stddef.h"

void *q3_memcpy(void *dst, const void *src, size_t n);
void *q3_memmove(void *dst, const void *src, size_t n);
void *q3_memset(void *s, int c, size_t n);
int   q3_memcmp(const void *a, const void *b, size_t n);
size_t q3_strlen(const char *s);
char *q3_strcpy(char *dst, const char *src);
char *q3_strncpy(char *dst, const char *src, size_t n);
char *q3_strcat(char *dst, const char *src);
char *q3_strncat(char *dst, const char *src, size_t n);
int   q3_strcmp(const char *a, const char *b);
int   q3_strncmp(const char *a, const char *b, size_t n);
char *q3_strchr(const char *s, int c);
char *q3_strrchr(const char *s, int c);
char *q3_strstr(const char *hay, const char *needle);
char *q3_strdup(const char *s);
char *q3_strtok(char *s, const char *delim);
char *q3_strpbrk(const char *s, const char *accept);
size_t q3_strspn(const char *s, const char *accept);
size_t q3_strcspn(const char *s, const char *reject);
char *q3_strerror(int errnum);
int   q3_strcasecmp(const char *a, const char *b);
int   q3_strncasecmp(const char *a, const char *b, size_t n);

#define memcpy   q3_memcpy
#define memmove  q3_memmove
#define memset   q3_memset
#define memcmp   q3_memcmp
#define strlen   q3_strlen
#define strcpy   q3_strcpy
#define strncpy  q3_strncpy
#define strcat   q3_strcat
#define strncat  q3_strncat
#define strcmp   q3_strcmp
#define strncmp  q3_strncmp
#define strchr   q3_strchr
#define strrchr  q3_strrchr
#define strstr   q3_strstr
#define strdup   q3_strdup
#define strtok   q3_strtok
#define strpbrk  q3_strpbrk
#define strspn   q3_strspn
#define strcspn  q3_strcspn
#define strerror q3_strerror
#define strcasecmp  q3_strcasecmp
#define strncasecmp q3_strncasecmp

#endif /* Q3STUB_STRING_H */
