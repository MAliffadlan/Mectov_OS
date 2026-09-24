/* q3_kernel.c — libc + Sys_* implementations for the ioquake3 subset on
 * Mectov OS kernel (v38.98). Every q3_* symbol here is referenced ONLY by
 * ioq3 object files (via the -I stub headers), so there is zero collision
 * with the kernel's own libc (utils.c) or doom_libc.c.
 *
 * Backing services:
 *   - heap: kmalloc/kfree (bounded by KERNEL_RESERVED_PAGES budget)
 *   - files: Mectov VFS (q3_FILE wraps a node index + cursor)
 *   - time: get_ticks() (PIT ms counter)
 *   - print: write_serial_string
 */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <time.h>  /* stub: time_t/struct tm */
#include <stdio.h> /* stub: FILE + q3_* stdio decls (self-reference is fine) */

/* ===== kernel services ===== */
extern void *kmalloc(unsigned int size);
extern void kfree(void *p);
extern void *krealloc(void *ptr, unsigned int new_size);
extern unsigned int get_ticks(void);
extern void write_serial_string(const char *s);
extern void write_serial_hex(unsigned int v);
extern int vfs_get_node(const char *path);
extern int vfs_read_file_offset(int node, int offset, char *buf, int len);
extern int vfs_write_file_offset(int node, int offset, const char *buf, int len, int append);
extern int vfs_create_file(const char *path);
extern int vfs_delete_node(const char *path);
extern int vfs_mkdir(const char *path);
extern unsigned int vfs_get_file_size(int node);
/* Big single allocations are what the engine's hunk/zone ask for; when one
 * fails we want the allocator's own numbers in the log instead of a bare
 * "failed to allocate". (v38.105)
 *
 * This TU cannot include the kernel's mem.h, so the layout below mirrors
 * kmalloc_stats_t there field for field — keep the two in step. It is only
 * ever read, and only on a >=4MB allocation that already failed. */
typedef struct { unsigned int heap_base, heap_used, allocated, free_bytes, blocks, free_blocks, largest_free, allocs, frees, oom_count, canary_failures, magic_failures; } q3_heap_stats_t;
extern void kmalloc_get_stats(void *s);  /* kernel's kmalloc_stats_t — same layout */
extern unsigned int phys_reserved_bytes(void);
#define Q3_HEAP_BASE_BYTES (24u * 1024 * 1024)
extern int vfs_write_file(const char *path, const char *data, int size);
/* node -> "/name" path: kernel node table keeps a flat name; the Q3 layer
 * only ever writes whole-file to top-level config files, so the path is
 * reconstructed from the node name's parent chain in vfs itself. Here we
 * just need SOME stable key — use the node index encoded as a name. */
static const char *q3_path_of(int node) {
	static char pbuf[512];
	extern int vfs_get_abs_path(int node_idx, char *buf, int buf_size);
	if (vfs_get_abs_path(node, pbuf, sizeof(pbuf)) != 0) return 0;
	return pbuf;
}

/* ===== errno ===== */
static int q3_errno_val;
int *__q3_errno_loc(void) { return &q3_errno_val; }

/* ===== assert ===== */
void q3_assert_fail(const char *expr, const char *file, int line) {
	write_serial_string("[Q3] ASSERT FAILED: ");
	write_serial_string(expr);
	write_serial_string(" at ");
	write_serial_string(file);
	write_serial_string(":");
	/* line as decimal */
	char num[12];
	int v = line, i = 10; num[11] = 0;
	if (v == 0) num[i--] = '0';
	while (v) { num[i--] = '0' + (v % 10); v /= 10; }
	write_serial_string(&num[i + 1]);
	write_serial_string("\n");
	q3_abort();
}

/* ===== ctype ===== */
int q3_isdigit(int c)  { return c >= '0' && c <= '9'; }
int q3_isalpha(int c)  { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int q3_isalnum(int c)  { return q3_isalpha(c) || q3_isdigit(c); }
int q3_isspace(int c)  { return c == ' ' || (c >= 9 && c <= 13); }
int q3_isprint(int c)  { return c >= 32 && c <= 126; }
int q3_isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int q3_islower(int c)  { return c >= 'a' && c <= 'z'; }
int q3_isxdigit(int c) { return q3_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int q3_toupper(int c)  { return q3_islower(c) ? c - 32 : c; }
int q3_tolower(int c)  { return q3_isupper(c) ? c + 32 : c; }

/* ===== string ===== */
void *q3_memcpy(void *dst, const void *src, size_t n) {
	unsigned char *d = dst; const unsigned char *s = src;
	while (n--) *d++ = *s++;
	return dst;
}
void *q3_memmove(void *dst, const void *src, size_t n) {
	unsigned char *d = dst; const unsigned char *s = src;
	if (d < s) { while (n--) *d++ = *s++; }
	else { d += n; s += n; while (n--) *--d = *--s; }
	return dst;
}
void *q3_memset(void *s, int c, size_t n) {
	unsigned char *p = s;
	while (n--) *p++ = (unsigned char)c;
	return s;
}
int q3_memcmp(const void *a, const void *b, size_t n) {
	const unsigned char *x = a, *y = b;
	while (n--) { if (*x != *y) return *x - *y; x++; y++; }
	return 0;
}
size_t q3_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
char *q3_strcpy(char *dst, const char *src) { char *d = dst; while ((*d++ = *src++)) ; return dst; }
char *q3_strncpy(char *dst, const char *src, size_t n) {
	char *d = dst;
	while (n && *src) { *d++ = *src++; n--; }
	while (n--) *d++ = 0;
	return dst;
}
char *q3_strcat(char *dst, const char *src) {
	char *d = dst; while (*d) d++;
	while ((*d++ = *src++)) ;
	return dst;
}
char *q3_strncat(char *dst, const char *src, size_t n) {
	char *d = dst; while (*d) d++;
	while (n-- && *src) *d++ = *src++;
	*d = 0;
	return dst;
}
int q3_strcmp(const char *a, const char *b) {
	while (*a && *a == *b) { a++; b++; }
	return (unsigned char)*a - (unsigned char)*b;
}
int q3_strncmp(const char *a, const char *b, size_t n) {
	while (n && *a && *a == *b) { a++; b++; n--; }
	if (!n) return 0;
	return (unsigned char)*a - (unsigned char)*b;
}
char *q3_strchr(const char *s, int c) {
	for (; ; s++) {
		if (*s == (char)c) return (char *)s;
		if (!*s) return 0;
	}
}
char *q3_strrchr(const char *s, int c) {
	char *last = 0;
	for (; *s; s++) if (*s == (char)c) last = (char *)s;
	return last;
}
char *q3_strstr(const char *hay, const char *needle) {
	if (!*needle) return (char *)hay;
	for (; *hay; hay++) {
		const char *h = hay, *n = needle;
		while (*h && *n && *h == *n) { h++; n++; }
		if (!*n) return (char *)hay;
	}
	return 0;
}
char *q3_strdup(const char *s) {
	size_t n = q3_strlen(s) + 1;
	char *p = kmalloc((unsigned int)n);
	if (p) q3_memcpy(p, s, n);
	return p;
}
char *q3_strtok(char *s, const char *delim) {
	static char *next;
	if (s) next = s;
	if (!next) return 0;
	while (*next && q3_strchr(delim, *next)) next++;
	if (!*next) { next = 0; return 0; }
	char *tok = next;
	while (*next && !q3_strchr(delim, *next)) next++;
	if (*next) { *next = 0; next++; } else next = 0;
	return tok;
}
char *q3_strpbrk(const char *s, const char *accept) {
	for (; *s; s++) if (q3_strchr(accept, *s)) return (char *)s;
	return 0;
}
size_t q3_strspn(const char *s, const char *accept) {
	size_t n = 0;
	for (; *s && q3_strchr(accept, *s); s++) n++;
	return n;
}
size_t q3_strcspn(const char *s, const char *reject) {
	size_t n = 0;
	for (; *s && !q3_strchr(reject, *s); s++) n++;
	return n;
}
char *q3_strerror(int errnum) {
	(void)errnum;
	return "q3 error";
}
int q3_strcasecmp(const char *a, const char *b) {
	while (*a && q3_tolower((unsigned char)*a) == q3_tolower((unsigned char)*b)) { a++; b++; }
	return q3_tolower((unsigned char)*a) - q3_tolower((unsigned char)*b);
}
int q3_strncasecmp(const char *a, const char *b, size_t n) {
	while (n && *a && q3_tolower((unsigned char)*a) == q3_tolower((unsigned char)*b)) { a++; b++; n--; }
	if (!n) return 0;
	return q3_tolower((unsigned char)*a) - q3_tolower((unsigned char)*b);
}

/* ===== stdlib ===== */
void *q3_malloc(size_t size) {
	if (size == 0) size = 1;
	return kmalloc((unsigned int)size);
}
void *q3_calloc(size_t num, size_t size) {
	size_t total = num * size;
	void *p = q3_malloc(total ? total : 1);
	if (!p && total >= (4u * 1024 * 1024)) {
		q3_heap_stats_t st;
		unsigned int cap = phys_reserved_bytes() - Q3_HEAP_BASE_BYTES;
		for (unsigned int i = 0; i < sizeof(st); i++) ((unsigned char *)&st)[i] = 0;
		kmalloc_get_stats(&st);
		write_serial_string("[Q3] calloc FAILED size=");
		write_serial_hex((unsigned int)total);
		write_serial_string(" heap_used=");
		write_serial_hex(st.heap_used);
		write_serial_string(" heap_cap=");
		write_serial_hex(cap);
		write_serial_string(" largest_free=");
		write_serial_hex(st.largest_free);
		write_serial_string(" free=");
		write_serial_hex(st.free_bytes);
		write_serial_string(" oom=");
		write_serial_hex(st.oom_count);
		write_serial_string("\n");
	}
	if (p) q3_memset(p, 0, total);
	return p;
}
void *q3_realloc(void *ptr, size_t new_size) {
	if (!ptr) return q3_malloc(new_size);
	if (!new_size) { q3_free(ptr); return 0; }
	/* block_meta: [magic u32][size u32][free int][next ptr] = 16 bytes */
	unsigned int old = *(unsigned int *)((unsigned char *)ptr - 8);
	void *p = q3_malloc(new_size);
	if (p) { q3_memcpy(p, ptr, old < (unsigned int)new_size ? old : (unsigned int)new_size); q3_free(ptr); }
	return p;
}
void q3_free(void *p) { if (p) kfree(p); }
void q3_exit(int status) {
	(void)status;
	write_serial_string("[Q3] exit() called — stopping engine\n");
	for (;;) __asm__ __volatile__("hlt");
}
void q3_abort(void) {
	write_serial_string("[Q3] abort() called\n");
	for (;;) __asm__ __volatile__("hlt");
}
static int q3_atoi_impl(const char *s) {
	int neg = 0, r = 0;
	while (q3_isspace(*s)) s++;
	if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
	while (q3_isdigit(*s)) r = r * 10 + (*s++ - '0');
	return neg ? -r : r;
}
int q3_atoi(const char *s) { return q3_atoi_impl(s); }
long q3_atol(const char *s) { return (long)q3_atoi_impl(s); }
double q3_atof(const char *s) {
	double r = 0.0; int neg = 0;
	while (q3_isspace(*s)) s++;
	if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
	while (q3_isdigit(*s)) r = r * 10.0 + (*s++ - '0');
	if (*s == '.') {
		s++;
		double frac = 0.1;
		while (q3_isdigit(*s)) { r += (*s++ - '0') * frac; frac *= 0.1; }
	}
	if (*s == 'e' || *s == 'E') {
		s++;
		int eneg = 0, e = 0;
		if (*s == '-') { eneg = 1; s++; } else if (*s == '+') s++;
		while (q3_isdigit(*s)) e = e * 10 + (*s++ - '0');
		double mul = 1.0;
		while (e--) mul *= 10.0;
		r = eneg ? r / mul : r * mul;
	}
	return neg ? -r : r;
}
static long q3_strtox(const char *s, char **endp, int base, int is_unsigned) {
	long r = 0; int neg = 0, any = 0;
	while (q3_isspace(*s)) s++;
	if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
	if ((base == 16 || base == 0) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
	else if (base == 0) base = s[0] == '0' ? 8 : 10;
	for (;;) {
		int d;
		if (q3_isdigit(*s)) d = *s - '0';
		else if (q3_isalpha(*s)) d = q3_tolower(*s) - 'a' + 10;
		else break;
		if (d >= base) break;
		r = r * base + d; any = 1; s++;
	}
	if (endp) *endp = (char *)(any ? s : s - 1);
	if (neg) r = -r;
	(void)is_unsigned;
	return r;
}
long q3_strtol(const char *s, char **endp, int base) { return q3_strtox(s, endp, base, 0); }
unsigned long q3_strtoul(const char *s, char **endp, int base) { return (unsigned long)q3_strtox(s, endp, base, 1); }
static unsigned int q3_rand_seed = 1;
int q3_rand(void) {
	q3_rand_seed = q3_rand_seed * 1103515245u + 12345u;
	return (int)((q3_rand_seed >> 16) & 0x7FFF);
}
void q3_srand(unsigned int seed) { q3_rand_seed = seed ? seed : 1; }
char *q3_getenv(const char *name) { (void)name; return 0; }
int q3_system(const char *cmd) { (void)cmd; return -1; }
static int q3_qsort_cmp_swap;
static void q3_qsort_rec(char *base, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
	if (n < 2) return;
	/* insertion sort — n is small in engine core paths */
	for (size_t i = 1; i < n; i++) {
		char tmp[64];
		if (sz > sizeof(tmp)) return;  /* refuse oversized elements */
		q3_memcpy(tmp, base + i * sz, sz);
		size_t j = i;
		while (j > 0 && cmp(base + (j - 1) * sz, tmp) > 0) {
			q3_memcpy(base + j * sz, base + (j - 1) * sz, sz);
			j--;
		}
		q3_memcpy(base + j * sz, tmp, sz);
	}
}
void q3_qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
	(void)q3_qsort_cmp_swap;
	q3_qsort_rec(base, n, sz, cmp);
}
int q3_abs(int x) { return x < 0 ? -x : x; }
long q3_labs(long x) { return x < 0 ? -x : x; }

/* ===== math (x87 via inline asm where it matters) ===== */
static inline double q3_fsin(double x) {
	double r;
	__asm__ __volatile__("fsin" : "=t"(r) : "0"(x));
	return r;
}
static inline double q3_fcos(double x) {
	double r;
	__asm__ __volatile__("fcos" : "=t"(r) : "0"(x));
	return r;
}
static inline double q3_fsqrt(double x) {
	double r;
	__asm__ __volatile__("fsqrt" : "=t"(r) : "0"(x));
	return r;
}
double q3_sin(double x) { return q3_fsin(x); }
double q3_cos(double x) { return q3_fcos(x); }
double q3_tan(double x) { return q3_fsin(x) / q3_fcos(x); }
double q3_sqrt(double x) { return q3_fsqrt(x); }
double q3_fabs(double x) { return x < 0 ? -x : x; }
/* v38.102: TinyGL's viewport clamp needs an isfinite() check (NaN/Inf -> 0).
 * Bit-level: NaN/Inf have an all-ones exponent field. int q3_isfinite(double). */
int q3_isfinite(double x) {
	union { double d; uint64_t u; } v;
	v.d = (double)x * 1.0;   /* quiet any signalling payload */
	v.u = (v.u & 0x7FF0000000000000ULL);
	return v.u != 0x7FF0000000000000ULL;
}
double q3_floor(double x) {
	long l = (long)x;
	return (x < 0 && (double)l != x) ? (double)(l - 1) : (double)l;
}
double q3_ceil(double x) {
	long l = (long)x;
	return (x > 0 && (double)l != x) ? (double)(l + 1) : (double)l;
}
double q3_fmod(double x, double y) {
	double q = x / y;
	return x - (double)((long)q) * y;
}
double q3_atan(double x) {
	/* range-reduced Taylor/identity: atan(x) = pi/2 - atan(1/x) for x>1 */
	const double PI_2 = 1.57079632679489661923;
	int neg = x < 0; if (neg) x = -x;
	int inv = x > 1.0; if (inv) x = 1.0 / x;
	double r = x, term = x, x2 = x * x;
	for (int i = 1; i < 24; i++) {
		term *= -x2;
		r += term / (2 * i + 1);
	}
	if (inv) r = PI_2 - r;
	return neg ? -r : r;
}
double q3_atan2(double y, double x) {
	if (x > 0) return q3_atan(y / x);
	if (x < 0 && y >= 0) return q3_atan(y / x) + 3.14159265358979323846;
	if (x < 0) return q3_atan(y / x) - 3.14159265358979323846;
	return (y > 0) ? 1.57079632679489661923 : ((y < 0) ? -1.57079632679489661923 : 0.0);
}
double q3_acos(double x) {
	/* Newton on cos(t)-x */
	double t = 1.57079632679489661923 - q3_atan(x / q3_fsqrt(1.0 - x * x + 1e-20));
	return t;
}
double q3_asin(double x) { return 1.57079632679489661923 - q3_acos(x); }
double q3_pow(double x, double y) {
	/* integer exponents only (engine core uses pow sparingly) */
	double r = 1.0;
	long n = (long)y;
	int inv = n < 0; if (inv) n = -n;
	while (n--) r *= x;
	return inv ? 1.0 / r : r;
}
double q3_exp(double x) {
	double r = 1.0, term = 1.0;
	for (int i = 1; i < 18; i++) { term *= x / i; r += term; }
	return r;
}
double q3_log(double x) {
	/* natural log via ln(x) = 2*atanh((x-1)/(x+1)) after range reduction */
	if (x <= 0) return 0.0;
	int k = 0;
	while (x > 2.0) { x /= 2.0; k++; }
	while (x < 1.0) { x *= 2.0; k--; }
	double t = (x - 1.0) / (x + 1.0), t2 = t * t, r = 0.0, term = t;
	for (int i = 0; i < 20; i++) { r += term / (2 * i + 1); term *= t2; }
	const double LN2 = 0.69314718055994530942;
	return 2.0 * r + k * LN2;
}
float q3_sqrtf(float x)  { return (float)q3_fsqrt((double)x); }
float q3_fabsf(float x)  { return x < 0 ? -x : x; }
float q3_floorf(float x) { return (float)q3_floor((double)x); }
float q3_ceilf(float x)  { return (float)q3_ceil((double)x); }
float q3_fmodf(float x, float y) { return (float)q3_fmod((double)x, (double)y); }
float q3_atan2f(float y, float x) { return (float)q3_atan2((double)y, (double)x); }
float q3_sinf(float x)   { return (float)q3_fsin((double)x); }
float q3_cosf(float x)   { return (float)q3_fcos((double)x); }

/* ===== time ===== */
time_t q3_time(time_t *t) {
	time_t r = (time_t)(get_ticks() / 1000);
	if (t) *t = r;
	return r;
}
static const char *q3_months[12] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
struct tm *q3_localtime(const time_t *t) {
	static struct tm tmv;
	time_t days = *t / 86400;
	time_t secs = *t % 86400;
	tmv.tm_hour = (int)(secs / 3600);
	tmv.tm_min = (int)((secs % 3600) / 60);
	tmv.tm_sec = (int)(secs % 60);
	tmv.tm_wday = (int)((days + 4) % 7);   /* epoch = Thursday */
	tmv.tm_year = 70; tmv.tm_mon = 0; tmv.tm_mday = (int)(days + 1);
	tmv.tm_yday = 0; tmv.tm_isdst = 0;
	(void)q3_months;
	return &tmv;
}
size_t q3_strftime(char *s, size_t max, const char *fmt, const struct tm *tmv) {
	/* minimal: %H:%M:%S only (q3 log format) */
	if (max < 9) return 0;
	s[0] = '0' + (tmv->tm_hour / 10) % 10;
	s[1] = '0' + tmv->tm_hour % 10;
	s[2] = ':';
	s[3] = '0' + (tmv->tm_min / 10) % 10;
	s[4] = '0' + tmv->tm_min % 10;
	s[5] = ':';
	s[6] = '0' + (tmv->tm_sec / 10) % 10;
	s[7] = '0' + tmv->tm_sec % 10;
	s[8] = 0;
	(void)fmt;
	return 8;
}

/* ===== stdio over VFS ===== */
struct q3_FILE {
	int   node;      /* VFS node index; -1 = closed/serial */
	int   pos;       /* byte cursor */
	int   size;      /* file size at open */
	int   err;       /* error flag */
	int   eof;       /* EOF flag */
	int   is_serial; /* stdout/stderr routing */
};
static struct q3_FILE q3_stdout_obj = { -1, 0, 0, 0, 0, 1 };
static struct q3_FILE q3_stderr_obj = { -1, 0, 0, 0, 0, 1 };
FILE *q3_stdout = &q3_stdout_obj;
FILE *q3_stderr = &q3_stderr_obj;

FILE *q3_fopen(const char *path, const char *mode) {
	int node = vfs_get_node(path);
	int want_write = q3_strchr(mode, 'w') || q3_strchr(mode, 'a') || q3_strchr(mode, '+');
	if (node < 0) {
		if (!want_write) return 0;
		if (vfs_create_file(path) < 0) return 0;
		node = vfs_get_node(path);
		if (node < 0) return 0;
	}
	struct q3_FILE *f = kmalloc(sizeof(struct q3_FILE));
	if (!f) return 0;
	f->node = node;
	f->pos = q3_strchr(mode, 'a') ? (int)vfs_get_file_size(node) : 0;
	f->size = (int)vfs_get_file_size(node);
	f->err = 0;
	f->eof = 0;
	f->is_serial = 0;
	return (FILE *)f;
}
int q3_fclose(FILE *fp) {
	struct q3_FILE *f = (struct q3_FILE *)fp;
	if (!f || f->is_serial) return 0;
	kfree(f);
	return 0;
}
size_t q3_fread(void *ptr, size_t size, size_t nmemb, FILE *fp) {
	struct q3_FILE *f = (struct q3_FILE *)fp;
	if (!f || f->is_serial || size == 0) return 0;
	size_t want = size * nmemb;
	if (f->pos >= f->size) { f->eof = 1; return 0; }
	size_t avail = (size_t)(f->size - f->pos);
	size_t take = want < avail ? want : avail;
	/* chunked read to bound stack/heap usage */
	unsigned char *dst = ptr;
	size_t done = 0;
	static char chunk[1024];
	while (done < take) {
		size_t step = take - done > sizeof(chunk) ? sizeof(chunk) : take - done;
		if (vfs_read_file_offset(f->node, f->pos + (int)done, chunk, (int)step) < 0) { f->err = 1; break; }
		q3_memcpy(dst + done, chunk, step);
		done += step;
	}
	f->pos += (int)done;
	if (f->pos >= f->size) f->eof = 1;
	return done / size;
}
size_t q3_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp) {
	struct q3_FILE *f = (struct q3_FILE *)fp;
	if (!f || f->is_serial || size == 0) return 0;
	size_t want = size * nmemb;
	const unsigned char *src = ptr;
	size_t done = 0;
	static char chunk[1024];
	while (done < want) {
		size_t step = want - done > sizeof(chunk) ? sizeof(chunk) : want - done;
		q3_memcpy(chunk, src + done, step);
		int _ow = vfs_write_file_offset(f->node, f->pos + (int)done, chunk, (int)step, 0);
		if (_ow < 0) {
			/* offset-write hanya mendukung FS_RAM_FILE; native FS_FILE
			 * ditulis whole-file via vfs_write_file (write-through disk). */
			if (done == 0) {
				static char whole[4096];
				size_t take = want < sizeof(whole) ? want : sizeof(whole);
				q3_memcpy(whole, src, take);
				if (vfs_write_file(q3_path_of(f->node), whole, (int)take) < 0) { f->err = 1; break; }
				done = take;
				f->pos = (int)take;
				if (f->pos > f->size) f->size = f->pos;
				return done / size;
			}
			f->err = 1; break;
		}
		done += step;
	}
	f->pos += (int)done;
	if (f->pos > f->size) f->size = f->pos;
	return done / size;
}
int q3_fseek(FILE *fp, long off, int whence) {
	struct q3_FILE *f = (struct q3_FILE *)fp;
	if (!f || f->is_serial) return -1;
	long np = f->pos;
	if (whence == SEEK_SET) np = off;
	else if (whence == SEEK_CUR) np += off;
	else np = (long)f->size + off;
	if (np < 0) return -1;
	f->pos = (int)np;
	f->eof = 0;
	return 0;
}
long q3_ftell(FILE *fp) {
	struct q3_FILE *f = (struct q3_FILE *)fp;
	return f ? (long)f->pos : -1;
}
int q3_feof(FILE *fp)   { struct q3_FILE *f = (struct q3_FILE *)fp; return f ? f->eof : 0; }
int q3_ferror(FILE *fp) { struct q3_FILE *f = (struct q3_FILE *)fp; return f ? f->err : 0; }
int q3_fflush(FILE *fp) { (void)fp; return 0; }
char *q3_fgets(char *buf, int size, FILE *fp) {
	struct q3_FILE *f = (struct q3_FILE *)fp;
	if (!f || size < 1) return 0;
	int i = 0;
	if (f->is_serial) {
		/* serial console lines are not interactive here */
		return 0;
	}
	while (i < size - 1 && f->pos < f->size) {
		char c;
		if (vfs_read_file_offset(f->node, f->pos, &c, 1) < 0) break;
		f->pos++;
		buf[i++] = c;
		if (c == '\n') break;
	}
	if (i == 0) { f->eof = 1; return 0; }
	buf[i] = 0;
	return buf;
}
int q3_ungetc(int c, FILE *fp) {
	struct q3_FILE *f = (struct q3_FILE *)fp;
	if (f && !f->is_serial && f->pos > 0) { f->pos--; return c; }
	return EOF;
}
int q3_remove(const char *path) { return vfs_delete_node(path) < 0 ? -1 : 0; }
int q3_rename(const char *old, const char *new) {
	/* Mectov VFS has no rename; emulate copy+delete for small files */
	int node = vfs_get_node(old);
	if (node < 0) return -1;
	unsigned int sz = vfs_get_file_size(node);
	if (sz > 256 * 1024) return -1;
	static char buf[256 * 1024];
	if (vfs_read_file_offset(node, 0, buf, (int)sz) < 0) return -1;
	if (vfs_create_file(new) < 0) return -1;
	int nn = vfs_get_node(new);
	if (nn < 0) return -1;
	if (vfs_write_file_offset(nn, 0, buf, (int)sz, 0) < 0) return -1;
	vfs_delete_node(old);
	return 0;
}
int q3_fputs(const char *s, FILE *fp) {
	struct q3_FILE *f = (struct q3_FILE *)fp;
	if (f && f->is_serial) { write_serial_string(s); return 0; }
	size_t n = q3_strlen(s);
	return q3_fwrite(s, 1, n, fp) == n ? 0 : EOF;
}
int q3_fputc(int c, FILE *fp) {
	char ch = (char)c;
	return q3_fwrite(&ch, 1, 1, fp) == 1 ? (unsigned char)c : EOF;
}
int q3_fgetc(FILE *fp) {
	struct q3_FILE *f = (struct q3_FILE *)fp;
	if (!f || f->is_serial) return EOF;
	if (f->pos >= f->size) { f->eof = 1; return EOF; }
	char c;
	if (vfs_read_file_offset(f->node, f->pos, &c, 1) < 0) return EOF;
	f->pos++;
	return (unsigned char)c;
}
void q3_rewind(FILE *fp) {
	struct q3_FILE *f = (struct q3_FILE *)fp;
	if (f) { f->pos = 0; f->eof = 0; }
}

/* ===== printf family (q3_vsnprintf lives in q3_printf.c) ===== */
int q3_snprintf(char *buf, size_t size, const char *fmt, ...) {
	va_list ap; va_start(ap, fmt);
	int r = q3_vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return r;
}
int q3_sprintf(char *buf, const char *fmt, ...) {
	va_list ap; va_start(ap, fmt);
	int r = q3_vsnprintf(buf, (size_t)-1, fmt, ap);
	va_end(ap);
	return r;
}
int q3_vprintf(const char *fmt, va_list ap) {
	char line[512];
	int r = q3_vsnprintf(line, sizeof(line), fmt, ap);
	if (r > 0) write_serial_string(line);
	return r;
}
int q3_printf(const char *fmt, ...) {
	va_list ap; va_start(ap, fmt);
	int r = q3_vprintf(fmt, ap);
	va_end(ap);
	return r;
}
int q3_vfprintf(FILE *fp, const char *fmt, va_list ap) {
	struct q3_FILE *f = (struct q3_FILE *)fp;
	if (f && f->is_serial) return q3_vprintf(fmt, ap);
	char line[512];
	int r = q3_vsnprintf(line, sizeof(line), fmt, ap);
	if (r > 0) q3_fwrite(line, 1, (size_t)r, fp);
	return r;
}
int q3_fprintf(FILE *fp, const char *fmt, ...) {
	va_list ap; va_start(ap, fmt);
	int r = q3_vfprintf(fp, fmt, ap);
	va_end(ap);
	return r;
}

/* NaN test for the official engine core (q_math.c / net_chan.c): a NaN is the
 * one value that is not equal to itself, which needs no bit fiddling. */
int q3_isnan(double x) {
	return x != x;
}

/* ===== unistd / misc ===== */
int q3_unlink(const char *path) { return q3_remove(path); }
int q3_getcwd(char *buf, size_t size) {
	if (size < 2) return -1;
	buf[0] = '/'; buf[1] = 0;
	return 0;
}
int q3_chdir(const char *path) { (void)path; return 0; }
