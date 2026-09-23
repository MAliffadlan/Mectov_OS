/* <ctype.h> stub — ioquake3 subset on Mectov OS kernel (v38.98).
 * Aliased to q3_* implementations in q3_kernel.c.
 */
#ifndef Q3STUB_CTYPE_H
#define Q3STUB_CTYPE_H

int q3_isdigit(int c);
int q3_isalpha(int c);
int q3_isalnum(int c);
int q3_isspace(int c);
int q3_isprint(int c);
int q3_isupper(int c);
int q3_islower(int c);
int q3_isxdigit(int c);
int q3_toupper(int c);
int q3_tolower(int c);

#define isdigit  q3_isdigit
#define isalpha  q3_isalpha
#define isalnum  q3_isalnum
#define isspace  q3_isspace
#define isprint  q3_isprint
#define isupper  q3_isupper
#define islower  q3_islower
#define isxdigit q3_isxdigit
#define toupper  q3_toupper
#define tolower  q3_tolower

#endif /* Q3STUB_CTYPE_H */
