// src/sys/shell/builtins/net_cmds/cmd_host_arg.c — the `host_arg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_host_arg(void) {
        char* domain = cmd_b + 5;
        if (!rtl_present) {
            print("No network card detected.\n", 0x0C);
        } else {
            print("Resolving ", 0x0B); print(domain, 0x0F); print(" ...\n", 0x0F);
            
            if (!net_ready) {
                net_send_arp_request(gateway_ip);
                if (!net_wait_for(&net_ready, 2000)) {
                    print("  [!] Gateway ARP timeout!\n", 0x0C);
                    goto host_done;
                }
            }

            net_send_dns_query(domain);
            if (net_wait_for(&dns_resolved, 3000)) {
                print(domain, 0x0A); print(" has address ", 0x0F);
                p_int(dns_resolved_ip[0], 0x0A); print(".", 0x0A);
                p_int(dns_resolved_ip[1], 0x0A); print(".", 0x0A);
                p_int(dns_resolved_ip[2], 0x0A); print(".", 0x0A);
                p_int(dns_resolved_ip[3], 0x0A); print("\n", 0x0A);
            } else {
                print("Host not found (timeout).\n", 0x0C);
            }
        }
        host_done: ;
}
