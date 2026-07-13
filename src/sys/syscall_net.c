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
            tcp_rx_len = 0;
            net_tcp_connect(ip, port);
            regs->eax = 0;
            break;
        }

        // ----- SYS_TCP_SEND (41) -----
        case SYS_TCP_SEND: {
            uint8_t* data = (uint8_t*)regs->ebx;
            int len = (int)regs->ecx;
            if (!validate_user_ptr(data, len)) { regs->eax = (uint32_t)-1; break; }
            net_tcp_send(data, len);
            regs->eax = 0;
            break;
        }

        // ----- SYS_TCP_RECV (42) -----
        case SYS_TCP_RECV: {
            uint8_t* buf = (uint8_t*)regs->ebx;
            int max_len = (int)regs->ecx;
            if (!validate_user_ptr(buf, max_len)) { regs->eax = (uint32_t)-1; break; }
            int copy = tcp_rx_len;
            if (copy > max_len) copy = max_len;
            if (copy > 0) {
                memcpy(buf, tcp_rx_buf, copy);
                tcp_rx_len = 0; // consumed
            }
            regs->eax = copy;
            break;
        }

        // ----- SYS_NET_STATUS (43) -----
        case SYS_NET_STATUS: {
            net_status_t* out = (net_status_t*)regs->ebx;
            if (validate_user_ptr(out, sizeof(net_status_t))) {
                out->dns_resolved = dns_resolved;
                for (int i = 0; i < 4; i++) out->dns_ip[i] = dns_resolved_ip[i];
                out->tcp_state = tcp_state;
            }
            regs->eax = 0;
            break;
        }

    }
    return regs->eax;
}
