/* q3_printf.c — routes ioquake3 printf-family formatting to the kernel's
 * proven vsnprintf (utils.c). Kept in its own TU because the stub stdio
 * header #defines vsnprintf -> q3_vsnprintf, which would rename the extern
 * declaration inside q3_kernel.c itself.
 */
#include <stddef.h>
#include <stdarg.h>

extern int vsnprintf(char *buf, unsigned int size, const char *fmt, void *va);

int q3_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
	return vsnprintf(buf, (unsigned int)size, fmt, (void *)ap);
}
