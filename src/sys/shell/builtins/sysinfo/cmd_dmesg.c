// src/sys/shell/builtins/sysinfo/cmd_dmesg.c — the `dmesg` shell command.
// Prints the kernel message ring buffer (the same text that goes to the
// serial port, captured by klog.c). klog_snapshot() returns the NEWEST
// KLOG_SIZE (64 KB) bytes, oldest-of-those first; the shell runs on a 16 KB
// kernel stack, so the snapshot goes through an 8 KB heap chunk (a real
// dmesg tail: everything if the log is smaller, else the most recent 8 KB).
#include "../../shell_internal.h"

void cmd_dmesg(void) {
    extern int klog_snapshot(char* dst, int max);
    extern int klog_bytes(void);
    extern void* kmalloc(uint32_t size);
    extern void kfree(void* ptr);

    char* chunk = (char*)kmalloc(8192);
    if (!chunk) {
        print("dmesg: out of memory\n", 0x0C);
        return;
    }

    int total = klog_bytes();
    int n = klog_snapshot(chunk, 8192);

    print("-- kernel log: ", 0x0D);
    p_int(total, 0x0E);
    print(" bytes --\n", 0x0D);
    for (int i = 0; i < n; i++) p_char(chunk[i], 0x0F);

    // Serial mirror so automated tests can verify dmesg output (terminal
    // output goes over IPC, not serial).
    extern void write_serial_string(const char*);
    extern void write_serial_hex(uint32_t);
    write_serial_string("[SH] dmesg total=");
    write_serial_hex((uint32_t)total);
    write_serial_string("\n");
    kfree(chunk);
}
