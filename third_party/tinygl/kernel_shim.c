/* kernel_shim.c — TinyGL allocator shim for the Mectov OS kernel.
 *
 * TinyGL routes every internal allocation through gl_malloc/gl_free (and a
 * couple of calloc calls in zbuffer.c) so a port can drop in its own
 * allocator — exactly the seam we need. The kernel's kmalloc heap is the
 * single allocator here; the same one the Q3 engine uses (q3_malloc), just
 * reached without the stub-header indirection because TinyGL compiles
 * against our shim headers, not the host libc.
 *
 * Includes are libc-shim style: this file is compiled with the Q3 stub
 * include path (-Ithird_party/q3mectov/stubs), so stdlib.h/string.h resolve
 * to the shims that alias malloc->q3_malloc (kmalloc) etc.
 */
#include <stdlib.h>   /* malloc/free/calloc (aliased to q3_* -> kmalloc) */
#include <string.h>   /* memset */

/* gl.h is namespaced via TGL_ADD_PREFIX; pull in the declarations we define. */
#include <TGL/gl.h>

void gl_free(void *p) {
    free(p);
}

void *gl_malloc(GLint size) {
    if (size <= 0) return NULL;
    return malloc((size_t)size);
}

/* Zeroed allocation used throughout the context/zbuffer init paths. */
void *gl_zalloc(GLint size) {
    if (size <= 0) return NULL;
    return calloc(1, (size_t)size);
}
