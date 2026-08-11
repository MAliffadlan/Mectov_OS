// src/sys/shell/builtins/net_cmds/cmd_ipconfig.c — the `ipconfig` shell
// command: print the current runtime network configuration — IP/netmask/
// gateway/DNS, whether it came from DHCP (v38.11) or the static fallback,
// and whether the gateway's MAC is resolved (link up).
#include "../../shell_internal.h"

// All network globals are declared in net.h (included via shell_internal.h):
// my_ip, gateway_ip, dns_server_ip, netmask_ip, dhcp_bound, net_ready.

static void print_ip4(const char* label, const uint8_t* ip) {
    print(label, 0x0B);
    p_int(ip[0], 0x0F); print(".", 0x0F);
    p_int(ip[1], 0x0F); print(".", 0x0F);
    p_int(ip[2], 0x0F); print(".", 0x0F);
    p_int(ip[3], 0x0F); print("\n", 0x0F);
}

void cmd_ipconfig(void) {
    if (!rtl_present) {
        print("ipconfig: no network card (RTL8139 not found)\n", 0x0C);
        return;
    }
    print("--- Network Configuration ---\n", 0x0B);
    print_ip4("IP:      ", my_ip);
    print_ip4("Netmask: ", netmask_ip);
    print_ip4("Gateway: ", gateway_ip);
    print_ip4("DNS:     ", dns_server_ip);
    print("Config:  ", 0x0B);
    if (dhcp_bound) print("DHCP (bound)\n", 0x0A);
    else            print("static\n", 0x0F);
    print("Link:    ", 0x0B);
    if (net_ready)  print("up (gateway resolved)\n", 0x0A);
    else            print("resolving...\n", 0x0F);
}
