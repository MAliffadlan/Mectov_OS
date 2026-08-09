// src/sys/shell/builtins/net_cmds/cmd_ping_arg.c — the `ping_arg` shell command.
// Extracted verbatim from the former monolithic src/sys/shell.c.
#include "../../shell_internal.h"

void cmd_ping_arg(void) {
        char* ip_str = cmd_b + 5;
        uint8_t tip[4] = {0, 0, 0, 0};
        int octet = 0, val = 0;
        for (int i = 0; ip_str[i] && octet < 4; i++) {
            if (ip_str[i] >= '0' && ip_str[i] <= '9') val = val * 10 + (ip_str[i] - '0');
            else if (ip_str[i] == '.') { tip[octet++] = (uint8_t)val; val = 0; }
        }
        if (octet < 4) tip[octet] = (uint8_t)val;
        
        if (!rtl_present) {
            print("No network card detected.\n", 0x0C);
        } else {
            print("PING ", 0x0B);
            p_int(tip[0], 0x0F); print(".", 0x0F);
            p_int(tip[1], 0x0F); print(".", 0x0F);
            p_int(tip[2], 0x0F); print(".", 0x0F);
            p_int(tip[3], 0x0F); print(" ...\n", 0x0F);
            
            if (!net_ready) {
                print("Resolving gateway (ARP)...\n", 0x07);
                net_send_arp_request(gateway_ip);
                if (!net_wait_for(&net_ready, 2000)) {
                    print("ARP timeout: gateway not found.\n", 0x0C);
                    goto ping_done;
                }
                print("Gateway resolved!\n", 0x0A);
            }

            net_send_ping(tip);
            if (net_wait_for(&ping_replied, 3000)) {
                uint32_t ms = (ping_rtt * 1000) / 60;
                print("Reply from ", 0x0A);
                p_int(tip[0], 0x0F); print(".", 0x0F);
                p_int(tip[1], 0x0F); print(".", 0x0F);
                p_int(tip[2], 0x0F); print(".", 0x0F);
                p_int(tip[3], 0x0F);
                print(" time=", 0x0F); p_int(ms, 0x0A); print("ms\n", 0x0F);
            } else {
                print("Request timed out.\n", 0x0C);
            }
        }
        ping_done: ;
}
