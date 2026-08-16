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

        // ============================================================
        // POSIX socket API (v38.43) — fd-integrated
        // ============================================================
        // ----- SYS_SOCKET (108) -----
        case SYS_SOCKET: {
            int domain = (int)regs->ebx;
            int type = (int)regs->ecx;
            if (domain != AF_INET ||
                (type != SOCK_STREAM && type != SOCK_DGRAM)) {
                regs->eax = (uint32_t)-1;
                break;
            }
            regs->eax = (uint32_t)sock_socket(type);
            break;
        }

        // ----- SYS_BIND (109) -----
        case SYS_BIND: {
            sockaddr_t* sa = (sockaddr_t*)regs->ecx;
            if (!validate_user_ptr(sa, sizeof(sockaddr_t))) { regs->eax = (uint32_t)-1; break; }
            regs->eax = (uint32_t)sock_bind((int)regs->ebx, sa->port);
            break;
        }

        // ----- SYS_LISTEN (110): backlog is the free-slot count -----
        case SYS_LISTEN: {
            int lfd = (int)regs->ebx;
            // The bind() port rides in the descriptor until consumed here.
            int port = 0;
            {
                int tid = get_current_task();
                extern global_fd_t global_fds[];
                if (tid < 0) { regs->eax = (uint32_t)-1; break; }
                int gfd = task_get_fd(tid, lfd);
                if (gfd < 0 || !global_fds[gfd].in_use ||
                    global_fds[gfd].type != FD_TYPE_SOCKET) { regs->eax = (uint32_t)-1; break; }
                port = global_fds[gfd].offset;
            }
            if (port <= 0) { regs->eax = (uint32_t)-1; break; }
            int conn = net_tcp_listen((uint16_t)port);
            if (conn < 0) { regs->eax = (uint32_t)-1; break; }
            sock_set_conn(lfd, conn);
            regs->eax = 0;
            break;
        }

        // ----- SYS_ACCEPT (111): non-blocking; poll(fd) for readiness -----
        case SYS_ACCEPT: {
            regs->eax = (uint32_t)sock_accept((int)regs->ebx);
            break;
        }

        // ----- SYS_CONNECT (112): starts the handshake; poll for POLLOUT -----
        case SYS_CONNECT: {
            sockaddr_t* sa = (sockaddr_t*)regs->ecx;
            if (!validate_user_ptr(sa, sizeof(sockaddr_t))) { regs->eax = (uint32_t)-1; break; }
            int conn = net_tcp_connect(sa->ip, sa->port);
            if (conn < 0) { regs->eax = (uint32_t)-1; break; }
            if (sock_set_conn((int)regs->ebx, conn) < 0) {
                net_tcp_close(conn);
                regs->eax = (uint32_t)-1;
                break;
            }
            regs->eax = 0;
            break;
        }

        // ----- SYS_SENDTO (113): stream ignores the address -----
        case SYS_SENDTO: {
            int lfd = (int)regs->ebx;
            uint8_t* buf = (uint8_t*)regs->ecx;
            int len = (int)regs->edx;
            sockaddr_t* sa = (sockaddr_t*)regs->esi;
            if (len <= 0 || !validate_user_ptr(buf, len)) { regs->eax = (uint32_t)-1; break; }
            if (sa && !validate_user_ptr(sa, sizeof(sockaddr_t))) { regs->eax = (uint32_t)-1; break; }
            if (len > 1400) len = 1400;

            int conn = sock_get_conn(lfd);
            if (conn == -2) { regs->eax = (uint32_t)-1; break; }   // not a socket fd
            if (sa && sa->port != 0) {
                // Destination given: UDP-style datagram send.
                regs->eax = (uint32_t)net_udp_send(sa->ip, sa->port, buf, (uint32_t)len);
                break;
            }
            if (conn < 0) { regs->eax = (uint32_t)-1; break; }
            regs->eax = (uint32_t)net_tcp_send(conn, buf, (uint32_t)len);
            break;
        }

        // ----- SYS_RECVFROM (114): fills the optional source address -----
        case SYS_RECVFROM: {
            int lfd = (int)regs->ebx;
            uint8_t* buf = (uint8_t*)regs->ecx;
            int max_len = (int)regs->edx;
            uint8_t* src_ip = (uint8_t*)regs->esi;
            uint16_t* src_port = (uint16_t*)regs->edi;
            if (max_len <= 0 || !validate_user_ptr(buf, max_len)) { regs->eax = (uint32_t)-1; break; }
            if (src_ip && !validate_user_ptr(src_ip, 4)) { regs->eax = (uint32_t)-1; break; }
            if (src_port && !validate_user_ptr(src_port, 2)) { regs->eax = (uint32_t)-1; break; }

            int conn = sock_get_conn(lfd);
            if (conn == -2) { regs->eax = (uint32_t)-1; break; }   // not a socket fd
            if (conn < 0) {
                // UDP: one datagram per call; report the last peer.
                int n = net_udp_recv(buf, (uint32_t)max_len);
                if (n >= 0) {
                    uint8_t peer_ip[4];
                    uint16_t peer_port = 0;
                    net_udp_peer(peer_ip, &peer_port);
                    if (src_ip) for (int i = 0; i < 4; i++) src_ip[i] = peer_ip[i];
                    if (src_port) *src_port = peer_port;
                }
                regs->eax = (uint32_t)n;
                break;
            }
            int r = net_tcp_recv(conn, buf, (uint32_t)max_len);
            regs->eax = (uint32_t)((r == -2) ? -1 : r);
            break;
        }

    }
    return regs->eax;
}
