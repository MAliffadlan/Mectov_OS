// ============================================================
// gdb_stub.c — In-kernel GDB Remote Serial Protocol stub (COM2)
// ============================================================
// Lets you debug the running kernel with a real GDB:
//
//   qemu-system-i386 ... -serial file:serial_debug.log
//       -serial tcp:127.0.0.1:2345,server=on,wait=off
//   gdb myos.bin -ex "target remote :2345" -ex "c"
// Then press F12 in the guest to break into the debugger. Software
// breakpoints (Z0/z0, int3 based) are supported, as well as single
// stepping (TF flag). While stopped, the system is frozen inside the
// trap ISR (interrupts off) — the timer cannot preempt the debugger.
//
// Design notes:
//  * All I/O is bounded-polling, never blocking forever. If no GDB is
//    connected, a trap resumes execution after a short timeout so a
//    stray int3 can never hang the kernel permanently.
//  * Memory reads/writes are clamped to the identity-mapped RAM region
//    (0..256MB) plus the framebuffer, so a wild GDB probe can never
//    page-fault inside an ISR.
// ============================================================
#include "../include/gdb_stub.h"
#include "../include/io.h"
#include "../include/serial.h"
#include "../include/utils.h"
#include "../include/vga.h"

#define GDB_COM 0x2F8  // COM2 (COM1 carries the kernel debug log)

// ---- COM2 UART (mirror of serial.c, different base port) ----
static void gdb_uart_init(void) {
    outb(GDB_COM + 1, 0x00);    // disable interrupts
    outb(GDB_COM + 3, 0x80);    // DLAB on
    outb(GDB_COM + 0, 0x03);    // divisor 3 -> 38400 baud
    outb(GDB_COM + 1, 0x00);
    outb(GDB_COM + 3, 0x03);    // 8N1
    outb(GDB_COM + 2, 0xC7);    // FIFO on
    outb(GDB_COM + 4, 0x0B);    // IRQs off, RTS/DSR set
}

static int gdb_rx_ready(void) {
    uint8_t st = inb(GDB_COM + 5);
    if (st == 0xFF) return 0;
    return st & 1;
}

static int gdb_tx_ready(void) {
    uint8_t st = inb(GDB_COM + 5);
    if (st == 0xFF) return 0;
    return st & 0x20;
}

static void gdb_putc(char c) {
    int guard = 100000;
    while (!gdb_tx_ready() && --guard > 0) { }
    outb(GDB_COM, (uint8_t)c);
}

static int gdb_getc(int* ok) {
    // Bounded wait: each inb on a QEMU serial port is a VM exit, so keep the
    // guard modest — the whole read then times out in roughly a second (KVM)
    // instead of minutes when no debugger is attached.
    int guard = 400000;
    while (!gdb_rx_ready() && --guard > 0) { }
    if (guard <= 0) { *ok = 0; return -1; }
    *ok = 1;
    return inb(GDB_COM);
}

// ---- Packet helpers (hex) ----
static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char hexch(uint8_t v) {
    return "0123456789abcdef"[v & 0x0F];
}

// ---- State ----
static int gdb_initialized = 0;
static int gdb_in_loop = 0;
static int gdb_pending_first = -1; // one byte pushed back by gdb_stub_poll

// ---- Software breakpoint table (int3 0xCC) ----
#define GDB_MAX_BP 16
typedef struct {
    uint32_t addr;
    uint8_t  orig;
    uint8_t  active;
} gdb_bp_t;
static gdb_bp_t gdb_bps[GDB_MAX_BP];

static gdb_bp_t* gdb_find_bp(uint32_t addr) {
    for (int i = 0; i < GDB_MAX_BP; i++)
        if (gdb_bps[i].active && gdb_bps[i].addr == addr) return &gdb_bps[i];
    return NULL;
}

static int gdb_insert_bp(uint32_t addr) {
    if (gdb_find_bp(addr)) return 1;
    for (int i = 0; i < GDB_MAX_BP; i++) {
        if (!gdb_bps[i].active) {
            gdb_bps[i].orig   = *(volatile uint8_t*)addr;
            *(volatile uint8_t*)addr = 0xCC;
            gdb_bps[i].addr   = addr;
            gdb_bps[i].active = 1;
            return 1;
        }
    }
    return 0; // table full
}

static int gdb_remove_bp(uint32_t addr) {
    gdb_bp_t* bp = gdb_find_bp(addr);
    if (!bp) return 1;
    *(volatile uint8_t*)addr = bp->orig;
    bp->active = 0;
    return 1;
}

// ---- Safe memory access (identity-mapped RAM + framebuffer only) ----
static int gdb_mem_ok(uint32_t addr, uint32_t len) {
    if (len == 0) return 1;
    if (addr < 256u * 1024 * 1024) {
        // identity map covers 0..256MB; also allow the framebuffer below
        if (addr + len <= 256u * 1024 * 1024) return 1;
        // fall through: maybe framebuffer
    }
    if (fb_addr && addr >= (uint32_t)fb_addr &&
        addr + len <= (uint32_t)fb_addr + (uint32_t)(fb_height * fb_pitch)) return 1;
    return 0;
}

// ---- Register packing (GDB i386 order) ----
// 0 eax, 1 ecx, 2 edx, 3 ebx, 4 esp, 5 ebp, 6 esi, 7 edi,
// 8 eip, 9 eflags, 10 cs, 11 ss, 12 ds, 13 es, 14 fs, 15 gs
static uint32_t gdb_get_reg(registers_t* r, int n) {
    switch (n) {
        case 0: return r->eax;
        case 1: return r->ecx;
        case 2: return r->edx;
        case 3: return r->ebx;
        case 4: return (r->cs & 3) ? r->useresp : r->esp;
        case 5: return r->ebp;
        case 6: return r->esi;
        case 7: return r->edi;
        case 8: return r->eip;
        case 9: return r->eflags;
        case 10: return r->cs;
        case 11: return r->ss;
        case 12: return r->ds;
        default: return 0; // es/fs/gs not tracked
    }
}

static void gdb_set_reg(registers_t* r, int n, uint32_t v) {
    switch (n) {
        case 0: r->eax = v; break;
        case 1: r->ecx = v; break;
        case 2: r->edx = v; break;
        case 3: r->ebx = v; break;
        case 4: if (r->cs & 3) r->useresp = v; else r->esp = v; break;
        case 5: r->ebp = v; break;
        case 6: r->esi = v; break;
        case 7: r->edi = v; break;
        case 8: r->eip = v; break;
        case 9: r->eflags = v; break;
        case 10: r->cs = v; break;
        case 11: r->ss = v; break;
        case 12: r->ds = v; break;
        default: break;
    }
}

// ---- Packet send / receive ----
static void gdb_send_packet(const char* data, int len) {
    uint8_t csum = 0;
    for (int i = 0; i < len; i++) csum += (uint8_t)data[i];
    gdb_putc('$');
    for (int i = 0; i < len; i++) gdb_putc(data[i]);
    gdb_putc('#');
    gdb_putc(hexch(csum >> 4));
    gdb_putc(hexch(csum));
}

// Receive one '$...#' packet into buf. Returns length, or -1 on timeout.
// Acknowledges with '+'/'-' per the RSP ACK protocol.
static int gdb_recv_packet(char* buf, int maxlen) {
    int ok = 0;
    int c = -1;
    int guard = 8000000; // ~a few seconds of polling

    // Consume a byte pushed back by gdb_stub_poll (already read & == '$').
    if (gdb_pending_first >= 0) {
        c = gdb_pending_first;
        gdb_pending_first = -1;
    }

    // skip junk until '$' (GDB may send 0x03 Ctrl-C; ignore here)
    while (guard-- > 0) {
        if (c < 0) c = gdb_getc(&ok);
        if (!ok) break;
        if (c == '$') break;
        c = -1;
    }
    if (c != '$') return -1;

    int len = 0;
    uint8_t csum = 0;
    while (len < maxlen) {
        c = gdb_getc(&ok);
        if (!ok) return -1;
        if (c == '#') break;
        buf[len++] = (char)c;
        csum += (uint8_t)c;
    }
    if (c != '#') return -1;

    // read 2 checksum hex digits
    int hi = -1, lo = -1;
    c = gdb_getc(&ok);
    if (ok) hi = hexval(c);
    c = gdb_getc(&ok);
    if (ok) lo = hexval(c);
    if (hi < 0 || lo < 0) return -1;

    if ((hi << 4 | lo) == csum) {
        gdb_putc('+'); // ACK
        return len;
    }
    gdb_putc('-'); // NAK, GDB will resend
    return -2;
}

// ---- Command dispatch ----
// Returns 1 when execution should resume (c/s/D), 0 to keep looping.
static int gdb_handle_packet(registers_t* r, char* pkt, int len) {
    char reply[512];
    int rl = 0;
    char c = pkt[0];

    if (len == 0) { gdb_send_packet("", 0); return 0; }

    switch (c) {
        case '?': // halt reason
            gdb_send_packet("S05", 3);
            return 0;

        case 'g': { // read all registers (little-endian byte order for i386)
            for (int i = 0; i < 16; i++) {
                uint32_t v = gdb_get_reg(r, i);
                // bytes least-significant first; within each byte high nibble first
                reply[rl++] = hexch(v >> 4);
                reply[rl++] = hexch(v);
                reply[rl++] = hexch(v >> 12);
                reply[rl++] = hexch(v >> 8);
                reply[rl++] = hexch(v >> 20);
                reply[rl++] = hexch(v >> 16);
                reply[rl++] = hexch(v >> 28);
                reply[rl++] = hexch(v >> 24);
            }
            gdb_send_packet(reply, rl);
            return 0;
        }

        case 'G': { // write all registers (little-endian byte order)
            if (len >= 16 * 8) {
                for (int i = 0; i < 16; i++) {
                    const char* b = pkt + 1 + i * 8;
                    uint32_t v = (uint32_t)((hexval(b[0]) << 4) | hexval(b[1])) |
                                 ((uint32_t)((hexval(b[2]) << 4) | hexval(b[3])) << 8) |
                                 ((uint32_t)((hexval(b[4]) << 4) | hexval(b[5])) << 16) |
                                 ((uint32_t)((hexval(b[6]) << 4) | hexval(b[7])) << 24);
                    gdb_set_reg(r, i, v);
                }
            }
            gdb_send_packet("OK", 2);
            return 0;
        }

        case 'p': { // read single register pNN (little-endian)
            int n = 0;
            int i = 1;
            while (i < len && hexval(pkt[i]) >= 0) { n = n * 16 + hexval(pkt[i]); i++; }
            uint32_t v = gdb_get_reg(r, n & 0x1F);
            reply[rl++] = hexch(v >> 4);
            reply[rl++] = hexch(v);
            reply[rl++] = hexch(v >> 12);
            reply[rl++] = hexch(v >> 8);
            reply[rl++] = hexch(v >> 20);
            reply[rl++] = hexch(v >> 16);
            reply[rl++] = hexch(v >> 28);
            reply[rl++] = hexch(v >> 24);
            gdb_send_packet(reply, rl);
            return 0;
        }

        case 'P': { // write single register PNN=value (little-endian)
            int n = 0;
            int i = 1;
            while (i < len && hexval(pkt[i]) >= 0) { n = n * 16 + hexval(pkt[i]); i++; }
            if (i < len && pkt[i] == '=') i++;
            if (i + 8 <= len) {
                uint32_t v = (uint32_t)((hexval(pkt[i]) << 4) | hexval(pkt[i + 1])) |
                             ((uint32_t)((hexval(pkt[i + 2]) << 4) | hexval(pkt[i + 3])) << 8) |
                             ((uint32_t)((hexval(pkt[i + 4]) << 4) | hexval(pkt[i + 5])) << 16) |
                             ((uint32_t)((hexval(pkt[i + 6]) << 4) | hexval(pkt[i + 7])) << 24);
                gdb_set_reg(r, n & 0x1F, v);
            }
            gdb_send_packet("OK", 2);
            return 0;
        }

        case 'm': { // read memory m addr,len
            uint32_t addr = 0;
            int i = 1;
            while (i < len && hexval(pkt[i]) >= 0) { addr = addr * 16 + hexval(pkt[i]); i++; }
            if (i < len && pkt[i] == ',') i++;
            uint32_t count = 0;
            while (i < len && hexval(pkt[i]) >= 0) { count = count * 16 + hexval(pkt[i]); i++; }
            // v38.53 fix: each byte becomes two hex chars in `reply`, so the
            // old `count <= 1024` bound let `m addr,400` write 2048 bytes
            // into reply[512] — a kernel-stack smash reachable by ANYONE
            // connected to COM2 (no auth). Bound it to the buffer instead;
            // real GDB splits large reads anyway.
            if (count == 0 || count > (uint32_t)(sizeof(reply) / 2) - 8 ||
                !gdb_mem_ok(addr, count)) {
                gdb_send_packet("E01", 3);
            } else {
                for (uint32_t k = 0; k < count; k++) {
                    uint8_t b = *(volatile uint8_t*)(addr + k);
                    reply[rl++] = hexch(b >> 4); reply[rl++] = hexch(b);
                }
                gdb_send_packet(reply, rl);
            }
            return 0;
        }

        case 'M': { // write memory M addr,len:data
            uint32_t addr = 0;
            int i = 1;
            while (i < len && hexval(pkt[i]) >= 0) { addr = addr * 16 + hexval(pkt[i]); i++; }
            if (i < len && pkt[i] == ',') i++;
            uint32_t count = 0;
            while (i < len && hexval(pkt[i]) >= 0) { count = count * 16 + hexval(pkt[i]); i++; }
            if (i < len && pkt[i] == ':') i++;
            if (count > 1024 || !gdb_mem_ok(addr, count)) {
                gdb_send_packet("E01", 3);
            } else {
                for (uint32_t k = 0; k < count; k++) {
                    int hi = hexval(pkt[i]); int lo = hexval(pkt[i + 1]);
                    if (hi < 0 || lo < 0) { gdb_send_packet("E02", 3); return 0; }
                    *(volatile uint8_t*)(addr + k) = (uint8_t)((hi << 4) | lo);
                    i += 2;
                }
                gdb_send_packet("OK", 2);
            }
            return 0;
        }

        case 'Z': // insert software breakpoint Z0,addr,kind
        case 'z': { // remove software breakpoint
            if (len < 2 || pkt[1] != '0') { gdb_send_packet("", 0); return 0; }
            uint32_t addr = 0;
            int i = 2;
            if (i < len && pkt[i] == ',') i++;
            while (i < len && hexval(pkt[i]) >= 0) { addr = addr * 16 + hexval(pkt[i]); i++; }
            // Breakpoints write 0xCC into guest memory, so the address must
            // pass the same memory-range clamp as m/M — a wild address would
            // page-fault inside the ISR.
            if (!gdb_mem_ok(addr, 1)) {
                gdb_send_packet("E01", 3);
                return 0;
            }
            if (c == 'Z') {
                if (gdb_insert_bp(addr)) gdb_send_packet("OK", 2);
                else gdb_send_packet("E08", 3);
            } else {
                gdb_remove_bp(addr);
                gdb_send_packet("OK", 2);
            }
            return 0;
        }

        case 'c': // continue
            return 1;
        case 's': // single step: set trap flag
            r->eflags |= 0x100;
            return 1;
        case 'D': // detach: ACK then resume (per RSP spec, OK goes first)
            gdb_send_packet("OK", 2);
            return 1;
        case 'k': // kill: stop the machine
            for (;;) __asm__ volatile("cli; hlt");
            return 1;

        case 'H': // thread ops — single threaded, always OK
        case '!': // extended mode
            gdb_send_packet("OK", 2);
            return 0;

        case 'q': { // query packets
            if (len >= 10 && strncmp(pkt, "qSupported", 10) == 0) {
                gdb_send_packet("PacketSize=400", 14);
            } else if (len >= 8 && strncmp(pkt, "qOffsets", 8) == 0) {
                gdb_send_packet("Text=0;Data=0;Bss=0", 18);
            } else if (len >= 9 && strncmp(pkt, "qAttached", 9) == 0) {
                gdb_send_packet("1", 1);
            } else if (len >= 2 && strncmp(pkt, "qC", 2) == 0) {
                gdb_send_packet("QC0", 3);
            } else {
                gdb_send_packet("", 0);
            }
            return 0;
        }

        default:
            gdb_send_packet("", 0);
            return 0;
    }
}

// ---- Main debugger loop ----
static void gdb_loop(registers_t* r) {
    gdb_in_loop = 1;
    char pkt[1024];
    int running = 0;
    while (!running) {
        int len = gdb_recv_packet(pkt, sizeof(pkt));
        if (len < 0) {
            // timeout or NAK: if GDB is not there, resume rather than hang
            if (len == -1) {
                write_serial_string("[GDB] no debugger, resuming\n");
                break;
            }
            continue;
        }
        running = gdb_handle_packet(r, pkt, len);
    }
    gdb_in_loop = 0;
}

// ---- Poll entry (GDB connect handshake while the OS is running) ----
// GDB sends packets immediately on `target remote`. If nobody answers, it
// errors out. The kernel main loop calls gdb_stub_poll() every iteration;
// when a '$' arrives we spin up a synthetic frame (only eip/eflags matter
// for the handshake; register contents are meaningless until the first real
// breakpoint) and run the same packet loop.
void gdb_stub_poll(void) {
    if (!gdb_initialized || gdb_in_loop) return;
    if (!gdb_rx_ready()) return;

    // Peek: is a GDB packet actually coming, or just noise? Read one byte;
    // if it is not '$', ignore it and let the poll spin again.
    int c = inb(GDB_COM);
    if (c != '$') return;

    registers_t synth;
    memset(&synth, 0, sizeof(synth));
    synth.cs = 0x08;
    synth.ds = 0x10;
    synth.ss = 0x10;
    synth.eflags = 0x202;
    synth.eip = 0x100000; // readable kernel text; replaced on first real trap
    synth.esp = 0;

    // Push the '$' back so gdb_recv_packet consumes it as the start of the
    // handshake packet. IMPORTANT: do NOT send an unsolicited S05 here — GDB
    // is mid-handshake and would mistake it for the reply to qSupported,
    // desyncing the whole session. It sends '?' itself; we answer then.
    gdb_pending_first = c;
    gdb_loop(&synth);
}

// ---- Trap entry (exceptions 1 and 3) ----
int gdb_stub_handle_trap(registers_t* r) {
    if (!gdb_initialized || gdb_in_loop) return 0;

    // Clear the trap flag from the saved EFLAGS: the CPU pushed EFLAGS with
    // TF set when the #DB fired, and iret would restore it — turning every
    // 'continue' into an endless single-step. The 's' command re-arms it.
    r->eflags &= ~0x100;

    write_serial_string("[GDB] trap int="); write_serial_hex(r->int_no);
    write_serial_string(" eip="); write_serial_hex(r->eip);
    write_serial_string(" cs="); write_serial_hex(r->cs);
    write_serial_string(" eflags="); write_serial_hex(r->eflags);
    write_serial_string("\n");

    // Software breakpoint hit: eip points AFTER the 0xCC. Restore the
    // original byte and back eip up so GDB shows the breakpoint location
    // and can step the real instruction before continuing.
    if (r->int_no == 3) {
        uint32_t bp = r->eip - 1;
        gdb_bp_t* b = gdb_find_bp(bp);
        if (b) {
            *(volatile uint8_t*)bp = b->orig;
            r->eip = bp;
        }
    }

    // Tell GDB we stopped (SIGTRAP) — it may already be connected.
    gdb_send_packet("S05", 3);

    gdb_loop(r);
    return 1;
}

void gdb_stub_break(void) {
    __asm__ __volatile__("int3");
}

void gdb_stub_init(void) {
    gdb_uart_init();
    gdb_initialized = 1;
    write_serial_string("[GDB] stub ready on COM2 (F12 to break)\n");
}
