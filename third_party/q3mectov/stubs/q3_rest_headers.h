/* errno / assert / signal / unistd / fcntl / sys / netinet / misc stubs
 * for the ioquake3 subset on Mectov OS (v38.98). The phase-1 subset never
 * executes most of these paths — they exist so compilation succeeds; the
 * q3_kernel.c implementations abort loudly if ever reached.
 */
#ifndef Q3STUB_ERRNO_H
#define Q3STUB_ERRNO_H
extern int *__q3_errno_loc(void);
#define errno (*__q3_errno_loc())
#define EINVAL 22
#define ENOENT 2
#define EEXIST 17
#define ENOMEM 12
#define EACCES 13
#define EINTR  4
#endif

#ifndef Q3STUB_ASSERT_H
#define Q3STUB_ASSERT_H
void q3_assert_fail(const char *expr, const char *file, int line);
#ifdef NDEBUG
#define assert(x) ((void)0)
#else
#define assert(x) ((x) ? (void)0 : q3_assert_fail(#x, __FILE__, __LINE__))
#endif
#endif

#ifndef Q3STUB_SIGNAL_H
#define Q3STUB_SIGNAL_H
typedef int sig_atomic_t;
#define SIGINT  2
#define SIGTERM 15
#endif

#ifndef Q3STUB_UNISTD_H
#define Q3STUB_UNISTD_H
#include "stddef.h"
int q3_unlink(const char *path);
int q3_getcwd(char *buf, size_t size);
int q3_chdir(const char *path);
#define unlink q3_unlink
#define getcwd q3_getcwd
#define chdir  q3_chdir
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif

#ifndef Q3STUB_FCNTL_H
#define Q3STUB_FCNTL_H
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0100
#define O_TRUNC  01000
#define O_APPEND 02000
#endif

#ifndef Q3STUB_SYS_STAT_H
#define Q3STUB_SYS_STAT_H
#include "stddef.h"
typedef unsigned int mode_t;
mode_t q3_umask(mode_t m);
#define umask q3_umask
#endif

#ifndef Q3STUB_SYS_TIME_H
#define Q3STUB_SYS_TIME_H
struct q3_timeval { long tv_sec; long tv_usec; };
typedef struct q3_timeval timeval;
#endif

#ifndef Q3STUB_SYS_MMAN_H
#define Q3STUB_SYS_MMAN_H
#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void*)-1)
#endif

#ifndef Q3STUB_SYS_TYPES_H
#define Q3STUB_SYS_TYPES_H
#include "stddef.h"
typedef int pid_t;
typedef long ssize_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef long off_t;
#endif

#ifndef Q3STUB_NETINET_IN_H
#define Q3STUB_NETINET_IN_H
#endif

#ifndef Q3STUB_DIRENT_H
#define Q3STUB_DIRENT_H
#endif

#ifndef Q3STUB_PTHREAD_H
#define Q3STUB_PTHREAD_H
#endif

#ifndef Q3STUB_DLFCN_H
#define Q3STUB_DLFCN_H
#endif

#ifndef Q3STUB_LOCALE_H
#define Q3STUB_LOCALE_H
#endif

#ifndef Q3STUB_SETJMP_H
#define Q3STUB_SETJMP_H
/* setjmp/longjmp via GCC builtin buffer. Phase-1 engine paths never
 * longjmp (Com_Error ERR_FATAL path lands in q3_abort instead). */
typedef long q3_jmp_buf_impl[24];
typedef q3_jmp_buf_impl jmp_buf;
int  q3_setjmp(jmp_buf env);
void q3_longjmp(jmp_buf env, int val);
#define setjmp(e)    q3_setjmp(e)
#define longjmp(e,v) q3_longjmp(e,v)
#endif
