/* sys_mectov.c — Mectov platform layer for the ioquake3 subset.
 * Host spike build (Q3_HOST_SPIKE): a plain main() that boots the engine
 * core with a null client and pumps Com_Frame, printing the banner.
 * Kernel build: doom_start()-style entry, same Sys_* implementations.
 */
#include "q3mectov.h"

#ifdef Q3_HOST_SPIKE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

static uint64_t host_msec_base;

uint64_t Sys_Mectov_Millis(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull;
}

/* Sys_Milliseconds — required by qcommon */
int Sys_Milliseconds(void) {
	return (int)(Sys_Mectov_Millis() - host_msec_base);
}

void Sys_Quit(void) {
	exit(0);
}

void Sys_Error(const char *error, ...) {
	va_list ap;
	fprintf(stderr, "Sys_Error: ");
	va_start(ap, error);
	vfprintf(stderr, error, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	exit(1);
}

int main(int argc, char **argv) {
	static char cmdline[256];
	strcpy(cmdline, "+set com_hunkMegs 48 +set fs_game baseq3 +set sv_mapname q3dm1");
	Com_Init(cmdline);
	printf("[Q3] engine core up, ticking Com_Frame...\n");
	for (int i = 0; i < 10; i++) {
		Com_Frame();
	}
	printf("[Q3] 10 Com_Frame ticks done — core alive\n");
	Com_Shutdown();
	return 0;
}
#endif

/* ===== Sys_* stubs required by the qcommon subset (spike) ===== */
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
char *Sys_DefaultInstallPath(void) { return "/tmp/q3base"; }
char *Sys_SteamPath(void) { return ""; }
char *Sys_GogPath(void) { return ""; }
char *Sys_MicrosoftStorePath(void) { return ""; }
qboolean Sys_OpenFolderInFileManager(const char *path, qboolean create) { return qfalse; }
void Sys_RemovePIDFile(const char *gamedir) { (void)gamedir; }
void Sys_InitPIDFile(const char *gamedir) { (void)gamedir; }
qboolean Sys_LowPhysicalMemory(void) { return qfalse; }
void Sys_SetEnv(const char *name, const char *value) { (void)name; (void)value; }
char *Sys_ConsoleInput(void) { return NULL; }
int Sys_PID(void) { return 1; }
qboolean Sys_PIDIsRunning(int pid) { (void)pid; return qfalse; }
qboolean Sys_RandomBytes(byte *string, int len) { (void)len; for (int i=0;i<len;i++) string[i]=rand(); return qtrue; }
qboolean Sys_DllExtension(const char *name) { (void)name; return qfalse; }
char *Sys_BinaryPath(void) { return "/tmp"; }
char *Sys_BinaryPathRelative(const char *relative) { static char buf[256]; snprintf(buf, sizeof(buf), "/tmp/%s", relative); return buf; }
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
qboolean Sys_Mkdir(const char *path) {
	if (mkdir(path, 0755) == 0) return qtrue;
	return errno == EEXIST;  /* existing dir = success */
}
const char *Sys_Dirname(char *path) { return path; }
const char *Sys_Basename(char *path) { return path; }
qboolean Sys_DllSynonym(const char *filename, char *dllpath, int dllpath_size) { (void)filename; (void)dllpath; (void)dllpath_size; return qfalse; }
void *Sys_LoadDll(const char *name, intptr_t *entryPoint) { (void)name; (void)entryPoint; return NULL; }
void Sys_UnloadDll(void *dllHandle) { (void)dllHandle; }
void Sys_ListFilesCallback(const char *name) { (void)name; }
char **Sys_ListFiles(const char *directory, const char *extension, char *filter, int *numfiles, qboolean wantsubs) { (void)directory; (void)extension; (void)filter; (void)wantsubs; if (numfiles) *numfiles = 0; return NULL; }
void Sys_FreeFileList(char **list) { (void)list; }
int Sys_GetDLLName(char *name, char *buffer, int size) { (void)name; (void)buffer; (void)size; return 0; }

/* ===== zlib stubs (spike tidak membuka .pk3; kernel port bundles miniz) ===== */
#include <stdlib.h>
unsigned long crc32(unsigned long crc, const unsigned char *buf, unsigned int len) { (void)crc; (void)buf; (void)len; abort(); }
int inflateInit2_(void *s, int a, const char *v, int sz) { (void)s; (void)a; (void)v; (void)sz; abort(); }
int inflate(void *s, int f) { (void)s; (void)f; abort(); }
int inflateEnd(void *s) { (void)s; abort(); }
/* ===== file paths (spike: /tmp sandbox) ===== */
#include <stdio.h>
FILE *Sys_FOpen(const char *ospath, const char *mode) { return fopen(ospath, mode); }
FILE *Sys_Mkfifo(const char *ospath) { (void)ospath; return NULL; }
char *Sys_DefaultHomePath(void) { return "/tmp/q3home"; }
char *Sys_DefaultHomeConfigPath(void) { return "/tmp/q3home"; }
char *Sys_DefaultHomeDataPath(void) { return "/tmp/q3home"; }
char *Sys_DefaultHomeStatePath(void) { return "/tmp/q3home"; }
/* ===== client-side symbols yang dihilangkan null client ===== */
int c_traces, c_brush_traces, c_patch_traces, c_pointcontents;
void Key_KeynameCompletion(void (*callback)(const char *s)) { (void)callback; }
/* ===== stub fase spike: VM + server + UI (server beneran menyusul) ===== */
void VM_Init(void) {}
void SV_Init(void) {}
void SV_Frame(int msec) { (void)msec; }
int SV_FrameMsec(void) { return 16; }
int SV_SendQueuedPackets(void) { return 0; }
void SV_Shutdown(char *finalmsg) { (void)finalmsg; }
qboolean UI_usesUniqueCDKey(void) { return qfalse; }
/* ===== x87 float helpers (versi C; kernel port pakai inline asm) ===== */
long qftolx87(float f) { return (long)f; }
int qvmftolx87(void) { return (int)qftolx87(0); }  /* spike only */
void qsnapvectorx87(vec3_t vec) {
	vec[0] = (float)((int)vec[0]);
	vec[1] = (float)((int)vec[1]);
	vec[2] = (float)((int)vec[2]);
}
void Sys_Init(void) {}
/* ===== SSE variants (spike: fitur dilaporkan 0, x87 path terpilih) ===== */
#include <stdint.h>
cpuFeatures_t Sys_GetProcessorFeatures(void) { return (cpuFeatures_t)0; }
long qftolsse(float f) { return (long)f; }
int qvmftolsse(void) { return 0; }
void qsnapvectorsse(vec3_t vec) { qsnapvectorx87(vec); }
void SV_ShutdownGameProgs(void) {}
void CIN_CloseAllVideos(void) {}
void VM_Clear(void) {}
void SV_PacketEvent(netadr_t from, msg_t *msg) { (void)from; (void)msg; }
void VM_Forced_Unload_Start(void) {}
void VM_Forced_Unload_Done(void) {}
void CL_ShutdownCGame(void) {}
void CL_ShutdownUI(void) {}
qboolean SV_GameCommand(void) { return qfalse; }
/* Sys_Print: route engine output ke stdout (host) / serial (kernel port) */
void Sys_Print(const char *msg) { fputs(msg, stdout); fflush(stdout); }
