/* q3_platform.c — kernel platform layer for the ioquake3 subset on Mectov OS
 * (v38.98). Plays the role doomgeneric_mectov.c plays for DOOM: hosts the
 * engine's main flow. Phase 1 (this file): Com_Init + Com_Frame ticking with
 * the null client — the engine core boots, runs its command/cvar/filesystem
 * stack, and streams output to the serial log. Renderer/TinyGL arrives next.
 *
 * The shell command `q3` enters q3_start() and does not return (like doom
 * fullscreen): the OS effectively becomes the game host for its lifetime.
 * Com_Printf output flows through Sys_Print -> serial, so the CI test can
 * assert engine liveness from serial_debug.log.
 */
#include "../../src/include/utils.h"

/* ioq3 types (qcommon/q_shared.h — via relative include so the libc stub
 * headers do NOT shadow anything for this TU; only ioq3's own sources get
 * the stub -I flags). */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <setjmp.h>
#include "../q3/code/qcommon/q_shared.h"
#include "../q3/code/qcommon/qcommon.h"

/* ioq3 core entry points (qcommon/common.c) */
extern void Com_Init(char *commandLine);
extern void Com_Frame(void);
extern void Com_Shutdown(void);

/* q3_kernel.c services shared with this layer */
extern unsigned int get_ticks(void);
extern void write_serial_string(const char *s);

/* Sys_* required by the qcommon subset — kernel-flavoured versions.
 * (q3_kernel.c holds libc; this file holds the Sys_* platform calls.) */
#include <stdint.h>

void *Sys_HeapAlloc(unsigned int size);  /* fwd, below */

static unsigned int q3_msec_base;

int Sys_Milliseconds(void) {
	unsigned int now = get_ticks();
	return (int)(now - q3_msec_base);
}

void Sys_Quit(void) {
	write_serial_string("[Q3] Sys_Quit\n");
	Com_Shutdown();
	for (;;) __asm__ __volatile__("hlt");
}

void Sys_Error(const char *error, ...) {
	static char buf[1024];
	va_list ap;
	va_start(ap, error);
	vsnprintf(buf, sizeof(buf), error, ap);
	va_end(ap);
	write_serial_string("[Q3] Sys_Error: ");
	write_serial_string(buf);
	write_serial_string("\n");
	for (;;) __asm__ __volatile__("hlt");
}

void Sys_Init(void) {
	q3_msec_base = get_ticks();
	write_serial_string("[Q3] Sys_Init: Mectov platform (ticks base set)\n");
}

/* Sys_Print lives in q3_kernel.c? No — Com_Printf needs it and it is
 * platform-flavoured: route to serial here. */
void Sys_Print(const char *msg) {
	write_serial_string(msg);
}

/* Minimal Sys_* surface for the phase-1 subset (null client, no net, no VM):
 * everything below is inert by design. */
void Sys_Quit_(void) {}
void Sys_Mkdir_fail(const char *p) { (void)p; }
int Sys_PID(void) { return 1; }
int q3_platform_unused;  /* keep the TU non-empty for strict compilers */

/* ===== entry ===== */
void q3_start(void) {
	write_serial_string("[Q3] Starting Quake III Arena (ioq3 subset) on Mectov OS...\n");

	static char cmdline[128];
	static const char args[] =
		"+set dedicated 1 "      /* server-like core; no client/GL yet */
		"+set com_hunkMegs 20 "
		"+set com_zoneMegs 6 "
		"+set com_maxfps 0 "      /* 0: minMsec=1, no 1000/fps wait loop */
		"+set com_busyWait 1 "    /* NET_Sleep is a no-op here; busy-wait */
		"+set fs_game baseq3";
	for (int i = 0; i < (int)sizeof(args); i++) cmdline[i] = args[i];

	/* ioq3's FS startup fatal-errors without a default.cfg in the search
	 * path; seed a minimal one over the VFS. v38.99: homepath lives in
	 * tmpfs (/tmp/q3home) so all engine config I/O is RAM-resident
	 * (FS_RAM_FILE) — the q3config.cfg auto-save path fires FS_Write-style
	 * small whole-file writes every frame, and per-write ATA PIO +
	 * 256-sector vfs_save stalls the disk driver within ~a dozen cycles.
	 * tmpfs writes are offset-native with zero ATA involvement. */
	{
		extern int vfs_mkdir(const char *path);
		extern int vfs_create_file(const char *path);
		extern int vfs_write_file(const char *path, const char *data, int size);
		vfs_mkdir("/tmp/q3home");
		vfs_mkdir("/tmp/q3home/baseq3");
		/* vfs_write_file does NOT auto-create; the node must exist first. */
		if (vfs_create_file("/tmp/q3home/baseq3/default.cfg") >= 0)
			vfs_write_file("/tmp/q3home/baseq3/default.cfg",
			               "// mectov default cfg\n", 23);
	}

	Com_Init(cmdline);

	write_serial_string("[Q3] Com_Init returned — ticking Com_Frame x10\n");
	for (int i = 0; i < 10; i++) {
		write_serial_string("[Q3] tick\n");
		Cbuf_ExecuteText(EXEC_NOW, "writeconfig fooconfig.cfg\n");
		Com_Frame();
		write_serial_string("[Q3] tick-done\n");
	}
	write_serial_string("[Q3] 10 Com_Frame ticks done — engine core alive\n");
	/* Phase 1 stops here on purpose: the engine demonstrated liveness.
	 * Phase 2 mounts baseq3 data + real SV_Frame; phase 3 adds TinyGL. */
	q3_exit_marker();
}

/* v38.99: end-of-run parking. q3_start runs on a dedicated kernel task
 * forked by cmd_q3 — returning from it would pop a garbage address (the
 * syscall's user stack) into EIP and page-fault (observed: EIP=0x1EFDF034,
 * a leftover user VA). Park the task halted instead; Sys_Quit/Sys_Error
 * park the same way. */
void q3_exit_marker(void) {
	write_serial_string("[Q3] engine task parking (hlt) — core demo complete\n");
	for (;;) __asm__ __volatile__("hlt");
}

/* ===== NET_* stubs (phase 1: net_ip/net_chan tidak dicompile; dedicated
 * core tanpa jaringan). MSG_* format sendiri disediakan msg.c. ===== */
void NET_Sleep(int msec) { (void)msec; }
void NET_FlushPacketQueue(void) {}
qboolean NET_GetLoopPacket(netsrc_t sock, netadr_t *from, msg_t *msg) { (void)sock; (void)from; (void)msg; return qfalse; }

/* ===== Sys_* surface lengkap untuk subset (mirror sys_mectov.c host spike;
 * versi kernel: path di-map ke VFS Mectov, mkdir via vfs_mkdir) ===== */
extern int vfs_mkdir(const char *path);
extern int vfs_delete_node(const char *path);
extern unsigned int vfs_get_file_size(int node);

/* Sys_Milliseconds/Sys_Print/Sys_Init/Sys_Quit/Sys_Error live above. */

static qboolean q3_mkdir_uniq(const char *path) {
	if (!path || !*path) return qfalse;
	if (vfs_get_node(path) >= 0) return qtrue;   /* sudah ada = sukses */
	return vfs_mkdir(path) >= 0;
}

qboolean Sys_Mkdir(const char *path) { return q3_mkdir_uniq(path); }

char *Sys_DefaultInstallPath(void) { return "/baseq3"; }
char *Sys_SteamPath(void) { return ""; }
char *Sys_GogPath(void) { return ""; }
char *Sys_MicrosoftStorePath(void) { return ""; }
qboolean Sys_OpenFolderInFileManager(const char *path, qboolean create) { (void)path; (void)create; return qfalse; }
void Sys_RemovePIDFile(const char *gamedir) { (void)gamedir; }
void Sys_InitPIDFile(const char *gamedir) { (void)gamedir; }
qboolean Sys_LowPhysicalMemory(void) { return qfalse; }
void Sys_SetEnv(const char *name, const char *value) { (void)name; (void)value; }
char *Sys_ConsoleInput(void) { return NULL; }
qboolean Sys_RandomBytes(byte *string, int len) {
	/* entropy kernel ada; cukup xor ticks + iterasi (bukan jalur kripto) */
	unsigned int s = get_ticks() ^ 0x9E3779B9u;
	for (int i = 0; i < len; i++) {
		s = s * 1664525u + 1013904223u;
		string[i] = (byte)(s >> 24);
	}
	return qtrue;
}
qboolean Sys_DllExtension(const char *name) { (void)name; return qfalse; }
char *Sys_BinaryPath(void) { return "/"; }
char *Sys_BinaryPathRelative(const char *relative) {
	static char buf[256];
	snprintf(buf, sizeof(buf), "/%s", relative);
	return buf;
}
char *Sys_StripAppBundle(char *pwd) { return pwd; }
void Sys_GLimpSafeInit(void) {}
void Sys_GLimpInit(void) {}
void Sys_PlatformInit(void) {}
void Sys_PlatformExit(void) {}
void Sys_ErrorDialog(const char *error) { (void)error; }
void Sys_AnsiColorPrint(const char *msg) { (void)msg; }
void Sys_SigHandler(int signal) { (void)signal; }
void Sys_LaunchAutoupdater(int argc, char **argv) { (void)argc; (void)argv; }
char *Sys_ParseProtocolUri(const char *uri) { (void)uri; return NULL; }
qboolean Sys_SetMaxFileLimit(void) { return qtrue; }
const char *Sys_Dirname(char *path) { return path; }
const char *Sys_Basename(char *path) { return path; }
void *Sys_LoadDll(const char *name, intptr_t *entryPoint) { (void)name; (void)entryPoint; return NULL; }
void Sys_UnloadDll(void *dllHandle) { (void)dllHandle; }
char **Sys_ListFiles(const char *directory, const char *extension, char *filter, int *numfiles, qboolean wantsubs) {
	(void)directory; (void)extension; (void)filter; (void)wantsubs;
	if (numfiles) *numfiles = 0;
	return NULL;
}
void Sys_FreeFileList(char **list) { (void)list; }

/* homepath: satu tempat di VFS root — config/data/state menumpuk di situ */
char *Sys_DefaultHomePath(void) { return "/tmp/q3home"; }
char *Sys_DefaultHomeConfigPath(void) { return "/tmp/q3home"; }
char *Sys_DefaultHomeDataPath(void) { return "/tmp/q3home"; }
char *Sys_DefaultHomeStatePath(void) { return "/tmp/q3home"; }

FILE *Sys_FOpen(const char *ospath, const char *mode) {
	/* fs_homepath/basepath di sini adalah path VFS (/q3home/baseq3/...).
	 * fopen read-only gak bikin file — match fopen("r") semantik host. */
	FILE *fp = fopen(ospath, mode);
	return fp;
}
FILE *Sys_Mkfifo(const char *ospath) { (void)ospath; return NULL; }

/* ===== sisa simbol yang di host spike ada di TU lain ===== */
int c_traces, c_brush_traces, c_patch_traces, c_pointcontents;
void Key_KeynameCompletion(void (*callback)(const char *s)) { (void)callback; }
void VM_Init(void) {}
void SV_Init(void) {}
void SV_Frame(int msec) { (void)msec; }
int SV_FrameMsec(void) { return 16; }
int SV_SendQueuedPackets(void) { return 0; }
void SV_Shutdown(char *finalmsg) { (void)finalmsg; }
void SV_PacketEvent(netadr_t from, msg_t *msg) { (void)from; (void)msg; }
void SV_ShutdownGameProgs(void) {}
qboolean SV_GameCommand(void) { return qfalse; }
void CIN_CloseAllVideos(void) {}
void VM_Clear(void) {}
void VM_Forced_Unload_Start(void) {}
void VM_Forced_Unload_Done(void) {}
void CL_ShutdownCGame(void) {}
void CL_ShutdownUI(void) {}
qboolean UI_usesUniqueCDKey(void) { return qfalse; }
cpuFeatures_t Sys_GetProcessorFeatures(void) { return (cpuFeatures_t)0; }
long qftolx87(float f) { return (long)f; }
long qftolsse(float f) { return (long)f; }
int qvmftolx87(void) { return 0; }
int qvmftolsse(void) { return 0; }
void qsnapvectorx87(vec3_t vec) {
	vec[0] = (float)((int)vec[0]); vec[1] = (float)((int)vec[1]); vec[2] = (float)((int)vec[2]);
}
void qsnapvectorsse(vec3_t vec) { qsnapvectorx87(vec); }

/* zlib stubs — pk3 tidak dibuka di fase 1 (kernel port bakal bundle miniz) */
unsigned long crc32(unsigned long crc, const unsigned char *buf, unsigned int len) { (void)crc; (void)buf; (void)len; return 0; }
int inflateInit2_(void *s, int a, const char *v, int sz) { (void)s; (void)a; (void)v; (void)sz; return -2; }
int inflate(void *s, int f) { (void)s; (void)f; return -2; }
int inflateEnd(void *s) { (void)s; return -2; }

/* stdlib yang belum ada: strtod + asctime */
double strtod(const char *s, char **endp);
double strtod(const char *s, char **endp) { if (endp) *endp = (char *)s; return atof(s); }
char *asctime(const struct tm *tmv);
char *asctime(const struct tm *tmv) { (void)tmv; return "Thu Jan  1 00:00:00 1970\n"; }
void Netchan_Init(int qport) { (void)qport; }
void NET_Restart_f(void) {}
int q3_setjmp(jmp_buf env) { (void)env; return 0; }
void q3_longjmp(jmp_buf env, int val) { (void)env; (void)val; q3_abort(); }
mode_t q3_umask(mode_t m) { (void)m; return 0755; }
