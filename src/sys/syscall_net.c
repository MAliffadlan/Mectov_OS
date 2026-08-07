#include "../include/idt.h"
#include "../include/syscall.h"
#include "../include/task.h"
#include "../include/vfs.h"
#include "../include/net.h"
#include "../include/wm.h"
#include "../include/ipc.h"
#include "../include/serial.h"
#include "../include/utils.h"
#include "../include/keyboard.h"
#include "../include/mouse.h"
#include "../include/fd.h"

extern int validate_user_ptr(const void* ptr, uint32_t size);
extern int safe_strlen(const char* s, int max);
extern void print(const char* s, uint8_t color);

extern int get_win_index(int wid);
extern void push_event(int wid, int type, int x, int y, int key);
extern void win_draw_cb(int id, int cx, int cy, int cw, int ch);
extern void win_key_cb(int id, char c, uint8_t sc);
extern void win_mouse_cb(int id, int cx, int cy, int btn);

#define MAX_EVENTS 64
typedef struct {
    int type; // 1 = paint, 2 = key, 3 = mouse
    int x, y;
    int key;
} gui_event_t;

typedef struct {
    gui_event_t events[MAX_EVENTS];
    int head;
    int tail;
} win_event_queue_t;

typedef struct {
    int type; // 1 = rect, 2 = text
    int x, y, w, h;
    uint32_t color;
    char text[128];
} draw_cmd_t;

#define MAX_DRAW_CMDS 512
typedef struct {
    draw_cmd_t cmds[MAX_DRAW_CMDS];
    int count;
    int pending_count;
    draw_cmd_t pending_cmds[MAX_DRAW_CMDS];
} win_canvas_t;

extern win_event_queue_t win_queues[];
extern win_canvas_t win_canvases[];

uint32_t handle_syscall_net(registers_t* regs) {
    switch (regs->eax) {
        // ----- SYS_DNS_RESOLVE (39) -----
        case SYS_DNS_RESOLVE: {
            const char* domain = (const char*)regs->ebx;
            if (safe_strlen(domain, 128) < 0) { regs->eax = (uint32_t)-1; break; }
            write_serial_string("[SYSCALL] SYS_DNS_RESOLVE called for domain: ");
            write_serial_string(domain);
            write_serial_string("\n");
            dns_resolved = 0;
            net_send_dns_query(domain);
            regs->eax = 0;
            break;
        }


        // ----- SYS_TCP_CONNECT (40) -----
        case SYS_TCP_CONNECT: {
            uint8_t* ip = (uint8_t*)regs->ebx;
            uint16_t port = (uint16_t)regs->ecx;
            if (!validate_user_ptr(ip, 4)) { regs->eax = (uint32_t)-1; break; }
            regs->eax = (uint32_t)net_tcp_connect(ip, port);
            break;
        }

        // ----- SYS_TCP_SEND (41) -----
        case SYS_TCP_SEND: {
            uint8_t* data = (uint8_t*)regs->ebx;
            int len = (int)regs->ecx;
            int conn_id = (int)regs->edx;
            // Cap the payload to fit the stack send buffers in net.c (1500-byte
            // pkt/pseudo_buf: 1500 - 20 IP - 20 TCP = 1460 max). Larger lengths
            // previously overflowed the kernel stack from Ring 3. Cap FIRST so
            // the user-pointer check below only walks the range actually read.
            if (len <= 0) { regs->eax = (uint32_t)-1; break; }
            if (len > 1400) len = 1400;
            if (!validate_user_ptr(data, len)) { regs->eax = (uint32_t)-1; break; }
            regs->eax = (uint32_t)net_tcp_send(conn_id, data, (uint32_t)len);
            break;
        }

        // ----- SYS_TCP_RECV (42) -----
        case SYS_TCP_RECV: {
            uint8_t* buf = (uint8_t*)regs->ebx;
            int max_len = (int)regs->ecx;
            int conn_id = (int)regs->edx;
            if (max_len <= 0 || !validate_user_ptr(buf, max_len)) { regs->eax = (uint32_t)-1; break; }
            regs->eax = (uint32_t)net_tcp_recv(conn_id, buf, (uint32_t)max_len);
            break;
        }

        // ----- SYS_NET_STATUS (43) -----
        case SYS_NET_STATUS: {
            net_status_t* out = (net_status_t*)regs->ebx;
            if (validate_user_ptr(out, sizeof(net_status_t))) {
                out->dns_resolved = dns_resolved;
                for (int i = 0; i < 4; i++) out->dns_ip[i] = dns_resolved_ip[i];
                out->tcp_state = net_tcp_latest_state();
            }
            regs->eax = 0;
            break;
        }

        // ----- SYS_TCP_CLOSE (70) -----
        case SYS_TCP_CLOSE: {
            int conn_id = (int)regs->ebx;
            if (conn_id < 0 || conn_id >= TCP_MAX_CONNS) { regs->eax = (uint32_t)-1; break; }
            net_tcp_close(conn_id);
            regs->eax = 0;
            break;
        }

        // ----- SYS_TCP_LISTEN (85): passive open (server) -----
        case SYS_TCP_LISTEN: {
            uint16_t port = (uint16_t)regs->ebx;
            regs->eax = (uint32_t)net_tcp_listen(port);
            break;
        }

        // ----- SYS_UDP_BIND (67) -----
        case SYS_UDP_BIND: {
            uint16_t port = (uint16_t)regs->ebx;
            extern int net_udp_bind(uint16_t);
            regs->eax = (uint32_t)net_udp_bind(port);
            break;
        }

        // ----- SYS_UDP_SEND (68) -----
        case SYS_UDP_SEND: {
            uint8_t* ip = (uint8_t*)regs->ebx;
            uint16_t port = (uint16_t)regs->ecx;
            uint8_t* data = (uint8_t*)regs->edx;
            int len = (int)regs->esi;
            if (!validate_user_ptr(ip, 4) || len <= 0 ||
                !validate_user_ptr(data, len)) { regs->eax = (uint32_t)-1; break; }
            if (len > 1400) len = 1400;
            extern int net_udp_send(uint8_t*, uint16_t, void*, uint32_t);
            regs->eax = (uint32_t)net_udp_send(ip, port, data, (uint32_t)len);
            break;
        }

        // ----- SYS_UDP_RECV (69) -----
        case SYS_UDP_RECV: {
            uint8_t* buf = (uint8_t*)regs->ebx;
            int max_len = (int)regs->ecx;
            if (max_len <= 0 || !validate_user_ptr(buf, max_len)) { regs->eax = (uint32_t)-1; break; }
            extern int net_udp_recv(uint8_t*, uint32_t);
            regs->eax = (uint32_t)net_udp_recv(buf, (uint32_t)max_len);
            break;
        }

    }
    return regs->eax;
}
