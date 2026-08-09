// src/sys/shell/builtins/net_cmds/cmd_fetch_arg.c — the `fetch_arg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_fetch_arg(void) {
        char* domain = cmd_b + 6;
        if (!rtl_present) {
            print("No network card detected.\n", 0x0C);
        } else {
            print("Fetching ", 0x0B); print(domain, 0x0F); print(" ...\n", 0x0F);

            if (!net_ready) {
                print("Resolving gateway (ARP)...\n", 0x07);
                net_send_arp_request(gateway_ip);
                if (!net_wait_for(&net_ready, 2000)) {
                    print("  [!] Gateway ARP timeout!\n", 0x0C);
                    goto fetch_done;
                }
            }

            net_send_dns_query(domain);
            if (!net_wait_for(&dns_resolved, 3000)) {
                print("Host not found (timeout).\n", 0x0C);
                goto fetch_done;
            }

            int id = net_tcp_connect(dns_resolved_ip, 80);
            if (id < 0) {
                print("No free TCP connection slots.\n", 0x0C);
                goto fetch_done;
            }
            print("Connecting...\n", 0x07);

            // Wait for the handshake (bounded like net_wait_for: tick deadline
            // plus the spin cap for the interrupt-gate syscall path)
            {
                uint32_t start = get_ticks();
                uint32_t spins = 0;
                while (net_tcp_state(id) != TCP_ESTABLISHED) {
                    if ((get_ticks() - start) >= 5000 || ++spins >= NET_WAIT_MAX_SPINS) break;
                    net_poll();
                }
            }
            if (net_tcp_state(id) != TCP_ESTABLISHED) {
                print("Connection failed.\n", 0x0C);
                net_tcp_close(id);
                goto fetch_done;
            }

            // Build the GET request (bounded by req[256])
            char req[256];
            const char* pre = "GET / HTTP/1.1\r\nHost: ";
            const char* post = "\r\nConnection: close\r\n\r\n";
            int n = 0;
            while (pre[n]) { req[n] = pre[n]; n++; }
            for (int i = 0; domain[i] && n < 250; i++) req[n++] = domain[i];
            for (int i = 0; post[i] && n < 255; i++) req[n++] = post[i];
            req[n] = '\0';

            net_tcp_send(id, (uint8_t*)req, n);
            print("Connected! Response:\n", 0x0A);

            // Drain until EOF (peer FIN), 15s idle cap plus the spin bound
            char rbuf[1024];
            uint32_t start = get_ticks();
            uint32_t spins = 0;
            for (;;) {
                int r = net_tcp_recv(id, (uint8_t*)rbuf, 1024);
                if (r == -1) break; // EOF: gateway closed after the body
                if (r == -2) {      // connection reset / slot freed
                    print("\n[fetch] connection lost.\n", 0x0C);
                    break;
                }
                if (r > 0) {
                    rbuf[r] = '\0';
                    print(rbuf, 0x0F);
                    start = get_ticks();
                    spins = 0;
                } else if ((get_ticks() - start) >= 15000 || ++spins >= NET_WAIT_MAX_SPINS) {
                    print("\n[fetch] idle timeout.\n", 0x0C);
                    break;
                } else {
                    net_poll();
                }
            }
            print("\n[fetch] done.\n", 0x0E);
            net_tcp_close(id);
        }
        fetch_done: ;
}
