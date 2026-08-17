#include "../include/net.h"
#include "../include/rtl8139.h"
#include "../include/utils.h"
#include "../include/timer.h"
#include "../include/serial.h"
#include "../include/entropy.h"   // get_random_u32() — unpredictable TCP ISN


// Static defaults (QEMU slirp layout). A successful DHCP exchange overwrites
// all of these at runtime — see the DHCP client section below. `net_ready` is
// only set once the gateway's MAC is resolved via ARP, whatever the source of
// the config.
uint8_t my_ip[4]        = {10, 0, 2, 15};
uint8_t gateway_ip[4]   = {10, 0, 2, 2};
uint8_t gateway_mac[6]  = {0, 0, 0, 0, 0, 0};
uint8_t dns_server_ip[4] = {10, 0, 2, 3};
uint8_t netmask_ip[4]   = {255, 255, 255, 0};
int     net_ready       = 0;
int     dhcp_bound      = 0;
static char pending_dns_domain[128] = {0};
static uint8_t pending_tcp_ip[4] = {0, 0, 0, 0};
static uint16_t pending_tcp_port = 0;
static int pending_tcp_id = -1;
int     ping_replied    = 0;
uint32_t ping_rtt       = 0;
static uint32_t ping_sent_tick = 0;
static uint16_t ping_seq = 0;

int     dns_resolved    = 0;
uint8_t dns_resolved_ip[4] = {0, 0, 0, 0};
static uint16_t ip_id_counter = 1000;

// Byte-swap helpers for network byte order
uint16_t htons(uint16_t val) {
    return (val >> 8) | (val << 8);
}
uint16_t ntohs(uint16_t val) {
    return (val >> 8) | (val << 8);
}

uint32_t htonl(uint32_t val) {
    return ((val & 0xFF) << 24) | ((val & 0xFF00) << 8) | ((val >> 8) & 0xFF00) | ((val >> 24) & 0xFF);
}
uint32_t ntohl(uint32_t val) { return htonl(val); }

// TCP connections: a small fixed table (one slot per concurrent connection).
// Each connection keeps its own ports, sequence numbers, receive buffer and
// retransmit state, so the browser, the shell and future apps can all have
// sockets open at once without stomping on each other.
static tcp_conn_t tcp_conns[TCP_MAX_CONNS];
static int tcp_latest = -1;          // most recently created conn (SYS_NET_STATUS)
static uint16_t tcp_port_counter = 49152;

#define TCP_RETRANS_MS   6000
#define TCP_RETRANS_MAX  5
#define TCP_CONNECT_TIMEOUT_MS 10000

// Forward: kicks off the handshake on a reserved slot (also used by the
// queued-connect dispatch in net_handle_arp, which appears earlier in the file).
static void tcp_start_conn(tcp_conn_t* c, uint8_t* target_ip, uint16_t port);

// TCP Pseudo-header for checksum
typedef struct __attribute__((packed)) {
    uint8_t src_ip[4];
    uint8_t dst_ip[4];
    uint8_t zeros;
    uint8_t protocol;
    uint16_t tcp_len;
} tcp_pseudo_header_t;

// IP checksum (RFC 1071)
static uint16_t ip_checksum(void* data, uint32_t len) {
    uint32_t sum = 0;
    uint16_t* p = (uint16_t*)data;
    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len == 1) {
        sum += *(uint8_t*)p;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

// Send a raw Ethernet frame
static void net_send_eth(uint8_t* dst_mac, uint16_t ethertype, void* payload, uint32_t payload_len) {
    uint8_t frame[1514];
    eth_header_t* eth = (eth_header_t*)frame;

    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, rtl_mac, 6);
    eth->ethertype = htons(ethertype);

    memcpy(frame + sizeof(eth_header_t), payload, payload_len);

    uint32_t total = sizeof(eth_header_t) + payload_len;
    if (total < 60) total = 60; // Minimum Ethernet frame size

    rtl8139_send_packet(frame, total);
}

// Send ARP request: "Who has target_ip? Tell my_ip"
void net_send_arp_request(uint8_t* target_ip_addr) {
    write_serial_string("[NET] Sending ARP request to gateway...\n");
    arp_packet_t arp;
    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    arp.hw_type    = htons(1);       // Ethernet
    arp.proto_type = htons(0x0800);  // IPv4
    arp.hw_len     = 6;
    arp.proto_len  = 4;
    arp.opcode     = htons(ARP_REQUEST);
    memcpy(arp.sender_mac, rtl_mac, 6);
    memcpy(arp.sender_ip, my_ip, 4);
    memset(arp.target_mac, 0, 6);
    memcpy(arp.target_ip, target_ip_addr, 4);

    net_send_eth(broadcast, ETH_TYPE_ARP, &arp, sizeof(arp));
}

// Send ARP reply
static void net_send_arp_reply(uint8_t* target_mac, uint8_t* target_ip_addr) {
    arp_packet_t arp;
    arp.hw_type    = htons(1);
    arp.proto_type = htons(0x0800);
    arp.hw_len     = 6;
    arp.proto_len  = 4;
    arp.opcode     = htons(ARP_REPLY);
    memcpy(arp.sender_mac, rtl_mac, 6);
    memcpy(arp.sender_ip, my_ip, 4);
    memcpy(arp.target_mac, target_mac, 6);
    memcpy(arp.target_ip, target_ip_addr, 4);

    net_send_eth(target_mac, ETH_TYPE_ARP, &arp, sizeof(arp));
}

// Handle incoming ARP packet
static void net_handle_arp(arp_packet_t* arp) {
    uint16_t op = ntohs(arp->opcode);

    if (op == ARP_REQUEST) {
        // Someone is asking for our MAC
        if (arp->target_ip[0] == my_ip[0] && arp->target_ip[1] == my_ip[1] &&
            arp->target_ip[2] == my_ip[2] && arp->target_ip[3] == my_ip[3]) {
            net_send_arp_reply(arp->sender_mac, arp->sender_ip);
        }
    } else if (op == ARP_REPLY) {
        // Got a reply — check if it's our gateway
        if (arp->sender_ip[0] == gateway_ip[0] && arp->sender_ip[1] == gateway_ip[1] &&
            arp->sender_ip[2] == gateway_ip[2] && arp->sender_ip[3] == gateway_ip[3]) {
            write_serial_string("[NET] Received ARP reply! Gateway MAC resolved.\n");
            memcpy(gateway_mac, arp->sender_mac, 6);
            net_ready = 1;
            
            // Dispatch pending DNS query if queued
            if (pending_dns_domain[0] != '\0') {
                write_serial_string("[NET] Dispatching pending DNS query from queue...\n");
                net_send_dns_query(pending_dns_domain);
                pending_dns_domain[0] = '\0';
            }
            
            // Dispatch pending TCP connection if queued
            if (pending_tcp_port != 0 && pending_tcp_id >= 0 && pending_tcp_id < TCP_MAX_CONNS) {
                write_serial_string("[NET] Dispatching pending TCP connection from queue...\n");
                tcp_start_conn(&tcp_conns[pending_tcp_id], pending_tcp_ip, pending_tcp_port);
                pending_tcp_port = 0;
                pending_tcp_id = -1;
            }
        }
    }


}

// Handle incoming ICMP (inside an IP packet)
static void net_handle_icmp(ip_header_t* ip, uint8_t* icmp_data, uint32_t icmp_len) {
    if (icmp_len < sizeof(icmp_header_t)) return;
    icmp_header_t* icmp = (icmp_header_t*)icmp_data;

    if (icmp->type == ICMP_ECHO_REQUEST) {
        // Reply to ping — build response
        uint8_t pkt[1500];
        ip_header_t* rip = (ip_header_t*)pkt;
        icmp_header_t* ricmp = (icmp_header_t*)(pkt + sizeof(ip_header_t));

        // IP header
        rip->ver_ihl    = 0x45;
        rip->tos        = 0;
        rip->total_len  = htons(sizeof(ip_header_t) + icmp_len);
        rip->id         = htons(0x1234);
        rip->flags_frag = 0;
        rip->ttl        = 64;
        rip->protocol   = IP_PROTO_ICMP;
        rip->checksum   = 0;
        memcpy(rip->src_ip, my_ip, 4);
        memcpy(rip->dst_ip, ip->src_ip, 4);
        rip->checksum   = ip_checksum(rip, sizeof(ip_header_t));

        // Copy original ICMP data, change type to reply
        memcpy(ricmp, icmp_data, icmp_len);
        ricmp->type     = ICMP_ECHO_REPLY;
        ricmp->checksum = 0;
        ricmp->checksum = ip_checksum(ricmp, icmp_len);

        // Determine destination MAC — if src IP is gateway, use gateway_mac
        uint8_t* dst_mac = gateway_mac;
        // Simple: always send to the MAC that sent us the original packet
        // We can extract it from the ethernet header, but for simplicity use gateway
        net_send_eth(dst_mac, ETH_TYPE_IP, pkt, sizeof(ip_header_t) + icmp_len);

    } else if (icmp->type == ICMP_ECHO_REPLY) {
        // We got a pong!
        ping_replied = 1;
        ping_rtt = get_ticks() - ping_sent_tick;
    }
}

// ---- UDP API (Ring 3 accessible) ----
#define UDP_RX_BUF_SIZE 65536
static void net_send_udp(uint8_t* target_ip, uint16_t src_port, uint16_t dst_port, void* payload, uint32_t payload_len);
static uint8_t  udp_rx_buf[UDP_RX_BUF_SIZE];
static int      udp_rx_len = 0;
static uint16_t udp_bind_port = 0;      // 0 = unbound
static uint8_t  udp_last_src_ip[4] = {0,0,0,0};
static uint16_t udp_last_src_port = 0;

// Bind a local UDP port for receiving. Returns 0 on success, -1 if busy.
int net_udp_bind(uint16_t port) {
    if (port == 0) return -1;
    __asm__ volatile("cli");
    if (udp_bind_port != 0 && udp_bind_port != port) {
        __asm__ volatile("sti");
        return -1; // another port already bound (single-socket design)
    }
    udp_bind_port = port;
    udp_rx_len = 0;
    __asm__ volatile("sti");
    return 0;
}

// Send a UDP datagram from the bound port (or ephemeral if unbound).
int net_udp_send(uint8_t* target_ip, uint16_t dst_port, void* payload, uint32_t payload_len) {
    if (!rtl_present) return -1;
    if (payload_len > 1400) payload_len = 1400; // fits 1500-byte frame
    uint16_t src = udp_bind_port;
    if (src == 0) src = (uint16_t)(50000 + (get_ticks() % 1000));
    net_send_udp(target_ip, src, dst_port, payload, payload_len);
    return 0;
}

// Copy received datagram into caller buffer. Returns bytes copied, 0 if none.
int net_udp_recv(uint8_t* out, uint32_t max_len) {
    if (udp_rx_len <= 0) return 0;
    __asm__ volatile("cli");
    int copy = udp_rx_len;
    if (copy > (int)max_len) copy = (int)max_len;
    if (copy > 0) memcpy(out, udp_rx_buf, copy);
    udp_rx_len = 0; // single datagram per recv (datagram semantics)
    __asm__ volatile("sti");
    return copy;
}

// Sender info for the last received datagram (IP+port).
void net_udp_peer(uint8_t* ip_out, uint16_t* port_out) {
    memcpy(ip_out, udp_last_src_ip, 4);
    *port_out = udp_last_src_port;
}

// Raw UDP send with arbitrary source/destination IPs and an explicit
// destination MAC — DHCP needs 0.0.0.0 -> 255.255.255.255 broadcast BEFORE
// the stack is configured, so this path has no net_ready gate. Normal traffic
// funnels through net_send_udp() below.
static void net_send_udp_raw(uint8_t* src_ip, uint8_t* dst_ip, uint8_t* dst_mac,
                             uint16_t src_port, uint16_t dst_port,
                             void* payload, uint32_t payload_len) {
    if (!rtl_present || payload_len > 1400) return;

    uint8_t pkt[1500];
    ip_header_t* ip = (ip_header_t*)pkt;
    udp_header_t* udp = (udp_header_t*)(pkt + sizeof(ip_header_t));

    uint32_t udp_len = sizeof(udp_header_t) + payload_len;
    uint32_t total = sizeof(ip_header_t) + udp_len;

    ip->ver_ihl    = 0x45;
    ip->tos        = 0;
    ip->total_len  = htons(total);
    ip->id         = htons(ip_id_counter++);
    ip->flags_frag = 0;
    ip->ttl        = 64;
    ip->protocol   = IP_PROTO_UDP;
    ip->checksum   = 0;
    memcpy(ip->src_ip, src_ip, 4);
    memcpy(ip->dst_ip, dst_ip, 4);
    ip->checksum   = ip_checksum(ip, sizeof(ip_header_t));

    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->len      = htons(udp_len);
    udp->checksum = 0; // Optional in IPv4

    memcpy(pkt + sizeof(ip_header_t) + sizeof(udp_header_t), payload, payload_len);

    net_send_eth(dst_mac, ETH_TYPE_IP, pkt, total);
}

// UDP send (normal path: from our IP, out via the gateway MAC)
static void net_send_udp(uint8_t* target_ip, uint16_t src_port, uint16_t dst_port, void* payload, uint32_t payload_len) {
    if (!net_ready) return;
    net_send_udp_raw(my_ip, target_ip, gateway_mac, src_port, dst_port, payload, payload_len);
}

// TCP segment send helper (per-connection)
static void net_send_tcp_segment(tcp_conn_t* c, uint8_t flags, uint8_t* payload, uint32_t payload_len) {
    if (!net_ready) return;

    uint8_t pkt[1500];
    ip_header_t* ip = (ip_header_t*)pkt;
    tcp_header_t* tcp = (tcp_header_t*)(pkt + sizeof(ip_header_t));

    uint32_t tcp_len = sizeof(tcp_header_t) + payload_len;
    uint32_t total = sizeof(ip_header_t) + tcp_len;

    ip->ver_ihl    = 0x45;
    ip->tos        = 0;
    ip->total_len  = htons(total);
    ip->id         = htons(ip_id_counter++);
    ip->flags_frag = 0;
    ip->ttl        = 64;
    ip->protocol   = IP_PROTO_TCP;
    ip->checksum   = 0;
    memcpy(ip->src_ip, my_ip, 4);
    memcpy(ip->dst_ip, c->remote_ip, 4);
    ip->checksum   = ip_checksum(ip, sizeof(ip_header_t));

    tcp->src_port = htons(c->local_port);
    tcp->dst_port = htons(c->remote_port);
    tcp->seq      = htonl(c->seq);
    tcp->ack      = htonl(c->ack);
    tcp->data_offset_res = (sizeof(tcp_header_t) / 4) << 4;
    tcp->flags    = flags;
    uint32_t free_win = TCP_CONN_BUF - c->rx_len;
    if (free_win > 65535) free_win = 65535;
    tcp->window_size = htons((uint16_t)free_win);
    tcp->checksum = 0;
    tcp->urgent_ptr = 0;

    if (payload_len > 0) {
        memcpy(pkt + sizeof(ip_header_t) + sizeof(tcp_header_t), payload, payload_len);
    }

    // Compute TCP checksum with pseudo-header
    uint8_t pseudo_buf[1500];
    tcp_pseudo_header_t* psh = (tcp_pseudo_header_t*)pseudo_buf;
    memcpy(psh->src_ip, my_ip, 4);
    memcpy(psh->dst_ip, c->remote_ip, 4);
    psh->zeros = 0;
    psh->protocol = IP_PROTO_TCP;
    psh->tcp_len = htons((uint16_t)tcp_len);
    
    memcpy(pseudo_buf + sizeof(tcp_pseudo_header_t), tcp, tcp_len);
    tcp->checksum = ip_checksum(pseudo_buf, sizeof(tcp_pseudo_header_t) + tcp_len);
    // Write back computed checksum
    memcpy(pkt + sizeof(ip_header_t), tcp, sizeof(tcp_header_t));

    // Stamp the retransmit base ONLY on new-data segments (and SYN). Pure ACKs
    // must not reset it: otherwise inbound traffic that triggers an ACK keeps
    // postponing the retransmit of genuinely-lost data indefinitely.
    if (payload_len > 0 || (flags & (TCP_SYN | TCP_FIN))) {
        c->last_tx_tick = get_ticks();
    }
    net_send_eth(gateway_mac, ETH_TYPE_IP, pkt, total);
}

// Kick off the handshake on an already-reserved slot. Used directly when the
// network is ready, and from the ARP-reply dispatch for the queued connect.
static void tcp_start_conn(tcp_conn_t* c, uint8_t* target_ip, uint16_t port) {
    // Redirect HTTP port 80 to Mectov Web Gateway Proxy on host (10.0.2.2:8888)
    if (port == 80) {
        memcpy(c->remote_ip, gateway_ip, 4);
        c->remote_port = 8888;
        write_serial_string("[TCP] Redirecting HTTP port 80 to Web Gateway Proxy at 10.0.2.2:8888\n");
    } else {
        memcpy(c->remote_ip, target_ip, 4);
        c->remote_port = port;
    }
    // Unpredictable ISN (v38.52): CSPRNG output. The get_ticks fallback only
    // runs in the microseconds before boot seeding completes, so the ISN is
    // never a bare 0.
    c->seq = get_random_u32();
    if (c->seq == 0) c->seq = get_ticks() * 12345 + c->local_port;
    c->ack = 0;
    c->rx_len = 0;
    c->eof = 0;
    c->unacked_len = 0;
    c->retrans_count = 0;
    c->conn_start_tick = get_ticks();
    c->state = TCP_SYN_SENT;

    write_serial_string("[TCP] connecting, src port=");
    write_serial_hex(c->local_port);
    write_serial_string("\n");
    net_send_tcp_segment(c, TCP_SYN, 0, 0);
    c->seq++; // SYN consumes one sequence number
}

// Reserve a connection slot and start (or queue) the handshake.
// Returns the connection id (0..TCP_MAX_CONNS-1) or -1 on failure.
int net_tcp_connect(uint8_t* target_ip, uint16_t port) {
    int id = -1;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (!tcp_conns[i].in_use) { id = i; break; }
    }
    if (id < 0) {
        write_serial_string("[TCP] no free connection slot\n");
        return -1;
    }

    tcp_conn_t* c = &tcp_conns[id];
    memset(c, 0, sizeof(tcp_conn_t));
    c->in_use = 1;
    c->state = TCP_CLOSED;
    c->local_port = tcp_port_counter++;
    c->listen_parent = -1;   // not a listener-spawned child
    if (tcp_port_counter > 60000) tcp_port_counter = 49152; // dynamic range
    tcp_latest = id;

    if (!net_ready) {
        // Queue until the gateway MAC is resolved
        memcpy(pending_tcp_ip, target_ip, 4);
        pending_tcp_port = port;
        pending_tcp_id = id;
        net_send_arp_request(gateway_ip);
        return id;
    }
    tcp_start_conn(c, target_ip, port);
    return id;
}

// Passive open: reserve a slot and wait for one inbound connection on `port`.
// The slot starts in TCP_LISTEN; net_handle_tcp() walks it to SYN_RCVD on a
// matching SYN and to ESTABLISHED on the final ACK. Server apps then use
// net_tcp_recv/send/close identically to a client connection.
int net_tcp_listen(uint16_t port) {
    int id = -1;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (!tcp_conns[i].in_use) { id = i; break; }
    }
    if (id < 0) {
        write_serial_string("[TCP] listen: no free slot\n");
        return -1;
    }

    tcp_conn_t* c = &tcp_conns[id];
    memset(c, 0, sizeof(tcp_conn_t));
    c->in_use = 1;
    c->state = TCP_LISTEN;
    c->local_port = port;
    // Unpredictable SYN-ACK ISN (v38.52), same CSPRNG as the client path.
    c->seq = get_random_u32();
    if (c->seq == 0) c->seq = get_ticks() * 54321 + port;
    c->rx_len = 0;
    c->eof = 0;
    c->unacked_len = 0;
    c->retrans_count = 0;
    c->listen_parent = -1;
    c->accepted = 0;
    tcp_latest = id;

    write_serial_string("[TCP] listening on port=");
    write_serial_hex(port);
    write_serial_string(" id=");
    write_serial_hex(id);
    write_serial_string("\n");
    return id;
}

// Server accept (v38.43): hand out one ESTABLISHED child of this listener.
int net_tcp_accept(int listener_id) {
    if (listener_id < 0 || listener_id >= TCP_MAX_CONNS) return -1;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (tcp_conns[i].in_use && !tcp_conns[i].accepted &&
            tcp_conns[i].listen_parent == listener_id &&
            tcp_conns[i].state == TCP_ESTABLISHED) {
            tcp_conns[i].accepted = 1;
            return i;
        }
    }
    return -1;
}

// Non-destructive poll variant (POLLIN on a listening socket).
int net_tcp_accept_pending(int listener_id) {
    if (listener_id < 0 || listener_id >= TCP_MAX_CONNS) return 0;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (tcp_conns[i].in_use && !tcp_conns[i].accepted &&
            tcp_conns[i].listen_parent == listener_id &&
            tcp_conns[i].state == TCP_ESTABLISHED) {
            return 1;
        }
    }
    return 0;
}

// Send app data over an established connection. Tracks the segment for
// retransmission until the peer ACKs it.
int net_tcp_send(int id, uint8_t* payload, uint32_t len) {
    if (id < 0 || id >= TCP_MAX_CONNS || !tcp_conns[id].in_use) return -2;
    tcp_conn_t* c = &tcp_conns[id];
    if (c->state != TCP_ESTABLISHED) return -1;
    if (len > 1400) len = 1400;

    c->unacked_seq = c->seq;
    c->unacked_len = len;
    c->retrans_count = 0;
    if (len > 0) {
        memcpy(c->tx_pending, payload, len);
        c->tx_pending_len = (int)len;
    }
    net_send_tcp_segment(c, TCP_ACK | TCP_PSH, payload, len);
    c->seq += len;
    return (int)len;
}

// Copy received bytes into the caller's buffer. Returns bytes copied, 0 if
// nothing new yet, -1 when the connection is closed/EOF, -2 for a bad id.
int net_tcp_recv(int id, uint8_t* out, uint32_t max_len) {
    if (id < 0 || id >= TCP_MAX_CONNS || !tcp_conns[id].in_use) return -2;
    tcp_conn_t* c = &tcp_conns[id];
    if (c->state == TCP_CLOSED) return -1;
    if (c->rx_len <= 0) return c->eof ? -1 : 0;

    int copy = c->rx_len;
    if (copy > (int)max_len) copy = (int)max_len;
    memcpy(out, c->rx, copy);
    if (copy < c->rx_len) {
        memmove(c->rx, c->rx + copy, c->rx_len - copy);
        c->rx_len -= copy;
    } else {
        c->rx_len = 0;
    }
    return copy;
}

// Initiate a graceful close: FIN handshake, then the slot is freed when the
// peer acknowledges. FIN_WAIT_1 -> (ACK) -> FIN_WAIT_2 -> (FIN) -> free.
void net_tcp_close(int id) {
    if (id < 0 || id >= TCP_MAX_CONNS || !tcp_conns[id].in_use) return;
    tcp_conn_t* c = &tcp_conns[id];

    if (c->state == TCP_ESTABLISHED) {
        net_send_tcp_segment(c, TCP_ACK | TCP_FIN, 0, 0);
        c->seq++;
        c->state = TCP_FIN_WAIT_1;
    } else if (c->state == TCP_CLOSE_WAIT) {
        // Peer already closed; ACK + FIN and wait for the final ACK
        net_send_tcp_segment(c, TCP_ACK | TCP_FIN, 0, 0);
        c->seq++;
        c->state = TCP_LAST_ACK;
    } else {
        c->state = TCP_CLOSED;
        c->in_use = 0;
    }
}

// State of one connection (for callers that need to poll a specific id).
int net_tcp_state(int id) {
    if (id < 0 || id >= TCP_MAX_CONNS || !tcp_conns[id].in_use) return TCP_CLOSED;
    return tcp_conns[id].state;
}

// Poll helpers for the fd layer (v38.43).
int net_tcp_rx_pending(int id) {
    if (id < 0 || id >= TCP_MAX_CONNS || !tcp_conns[id].in_use) return 0;
    return tcp_conns[id].rx_len > 0;
}
int net_tcp_eof(int id) {
    if (id < 0 || id >= TCP_MAX_CONNS || !tcp_conns[id].in_use) return 0;
    return tcp_conns[id].eof;
}

// State of the most recently created connection (SYS_NET_STATUS compat).
int net_tcp_latest_state(void) {
    if (tcp_latest >= 0 && tcp_latest < TCP_MAX_CONNS && tcp_conns[tcp_latest].in_use) {
        return tcp_conns[tcp_latest].state;
    }
    return TCP_CLOSED;
}

// Convert "google.com" to "\x06google\x03com\x00"
static int format_dns_name(char* qname, const char* domain) {
    int qpos = 0;
    int len_pos = qpos++;
    int count = 0;
    for (int i = 0; domain[i]; i++) {
        if (domain[i] == '.') {
            qname[len_pos] = count;
            len_pos = qpos++;
            count = 0;
        } else {
            qname[qpos++] = domain[i];
            count++;
        }
    }
    qname[len_pos] = count;
    qname[qpos++] = 0;
    return qpos;
}

// Send DNS Query
void net_send_dns_query(const char* domain) {
    if (!net_ready) {
        write_serial_string("[NET] net_send_dns_query: Network not ready. Queuing domain: ");
        write_serial_string(domain);
        write_serial_string("\n");
        int i = 0;
        for (; i < 127 && domain[i]; i++) pending_dns_domain[i] = domain[i];
        pending_dns_domain[i] = '\0';
        net_send_arp_request(gateway_ip);
        return;
    }
    write_serial_string("[NET] Sending DNS query for domain: ");
    write_serial_string(domain);
    write_serial_string("\n");
    dns_resolved = 0;

    uint8_t payload[512];
    dns_header_t* dns = (dns_header_t*)payload;
    dns->id = htons(0xABCD);
    dns->flags = htons(0x0100); // Standard query, Recursion desired
    dns->qdcount = htons(1);
    dns->ancount = 0;
    dns->nscount = 0;
    dns->arcount = 0;

    int qpos = sizeof(dns_header_t);
    qpos += format_dns_name((char*)payload + qpos, domain);

    // QTYPE = 1 (A record)
    payload[qpos++] = 0; payload[qpos++] = 1;
    // QCLASS = 1 (IN)
    payload[qpos++] = 0; payload[qpos++] = 1;

    // DNS server: the configured one (DHCP option 6, else the QEMU slirp
    // default 10.0.2.3) which proxies to the host resolver.
    net_send_udp(dns_server_ip, 12345, 53, payload, qpos);
}

static void net_handle_dns(uint8_t* data, uint32_t len) {
    if (len < sizeof(dns_header_t)) {
        write_serial_string("[NET] net_handle_dns: packet too small\n");
        return;
    }
    dns_header_t* dns = (dns_header_t*)data;
    uint16_t dns_id = ntohs(dns->id);
    uint16_t dns_flags = ntohs(dns->flags);
    uint16_t qdcount = ntohs(dns->qdcount);
    uint16_t ancount = ntohs(dns->ancount);
    
    write_serial_string("[NET] net_handle_dns: id=");
    write_serial_hex(dns_id);
    write_serial_string(" flags=");
    write_serial_hex(dns_flags);
    write_serial_string(" qdcount=");
    write_serial_hex(qdcount);
    write_serial_string(" ancount=");
    write_serial_hex(ancount);
    write_serial_string("\n");

    if (dns_id != 0xABCD) {
        write_serial_string("[NET] net_handle_dns: ID mismatch!\n");
        return;
    }
    
    // Check if it's a response (QR bit)
    if (!(dns_flags & 0x8000)) {
        write_serial_string("[NET] net_handle_dns: not a response!\n");
        return;
    }

    if (ancount == 0) {
        write_serial_string("[NET] net_handle_dns: ancount is 0!\n");
        return;
    }


    int pos = sizeof(dns_header_t);
    
    // Skip questions
    for (int i = 0; i < qdcount; i++) {
        while (pos < (int)len && data[pos] != 0) {
            if ((data[pos] & 0xC0) == 0xC0) { pos += 2; break; } // Pointer
            pos += data[pos] + 1;
        }
        if (pos < (int)len && data[pos] == 0) pos++;
        pos += 4; // Skip QTYPE, QCLASS
    }

    // Parse answers
    for (int a = 0; a < ancount; a++) {
        if (pos >= (int)len) break;
        
        // Skip or parse name (can be a pointer or label)
        if ((data[pos] & 0xC0) == 0xC0) {
            pos += 2;
        } else {
            while (pos < (int)len && data[pos] != 0) pos += data[pos] + 1;
            pos++;
        }
        
        if (pos + 10 > (int)len) break;
        
        uint16_t type = (data[pos] << 8) | data[pos+1];
        pos += 8; // type(2), class(2), ttl(4)
        
        uint16_t rdlength = (data[pos] << 8) | data[pos+1];
        pos += 2;
        
        if (pos + rdlength > (int)len) break;
        
        if (type == 1 && rdlength == 4) {
            // A Record found!
            dns_resolved_ip[0] = data[pos];
            dns_resolved_ip[1] = data[pos+1];
            dns_resolved_ip[2] = data[pos+2];
            dns_resolved_ip[3] = data[pos+3];
            dns_resolved = 1;
            write_serial_string("[NET] DNS A-record resolved successfully!\n");
            break;
        }
        
        // Go to next record
        pos += rdlength;
    }
}

// ============================================================
// DHCP client (RFC 2131) — discover/offer/request/ack over UDP broadcast
// ============================================================
// On boot we have no address yet, so DISCOVER/REQUEST go out as UDP broadcast
// (0.0.0.0:68 -> 255.255.255.255:67, Ethernet FF:FF:FF:FF:FF:FF) via
// net_send_udp_raw — before net_ready is set. The server's OFFER/ACK may come
// back broadcast too, so the RX path accepts broadcast (and 0.0.0.0) frames
// while unbound. On ACK the runtime config (my_ip / gateway / DNS / netmask)
// is overwritten and the gateway MAC is re-resolved via ARP (which is what
// sets net_ready). If no server answers after a few retries we fall back to
// the static defaults, so networking still works without a DHCP server.
// The state machine is driven from net_poll() (main loop, inside its cli
// window); the IRQ path only parses and records OFFER/ACK data.

typedef struct __attribute__((packed)) {
    uint8_t  op;            // 1 = BOOTREQUEST, 2 = BOOTREPLY
    uint8_t  htype;         // 1 = Ethernet
    uint8_t  hlen;          // 6
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;         // 0x8000 = broadcast response requested
    uint8_t  ciaddr[4];     // client IP (bound state)
    uint8_t  yiaddr[4];     // 'your' (offered) IP
    uint8_t  siaddr[4];     // server IP
    uint8_t  giaddr[4];     // relay agent IP
    uint8_t  chaddr[16];    // client hardware address
    char     sname[64];
    char     file[128];
    uint8_t  magic[4];      // 0x63 0x82 0x53 0x63
    // options follow
} dhcp_packet_t;

#define DHCP_STATE_IDLE    0
#define DHCP_STATE_DISCOVER 1
#define DHCP_STATE_REQUEST  2
#define DHCP_STATE_BOUND    3

static int  dhcp_state = DHCP_STATE_IDLE;
static uint32_t dhcp_xid = 0;
static uint32_t dhcp_last_send_tick = 0;
static int  dhcp_retries = 0;
static uint8_t dhcp_offered_ip[4] = {0, 0, 0, 0};
static uint8_t dhcp_server_id[4]  = {0, 0, 0, 0};
static uint8_t dhcp_router[4]     = {0, 0, 0, 0};
static uint8_t dhcp_dns[4]        = {0, 0, 0, 0};
static uint8_t dhcp_mask[4]       = {0, 0, 0, 0};

#define DHCP_RETRY_MS    1000
#define DHCP_MAX_RETRIES 3

static void dhcp_send_discover(void);
static void dhcp_send_request(void);

// Append a 3-byte option (type, len, single byte value) to a DHCP message.
static int dhcp_opt3(uint8_t* p, int o, uint8_t type, uint8_t val) {
    p[o++] = type; p[o++] = 1; p[o++] = val;
    return o;
}

// Append an N-byte option (type, len, bytes).
static int dhcp_optN(uint8_t* p, int o, uint8_t type, const uint8_t* bytes, int n) {
    p[o++] = type; p[o++] = (uint8_t)n;
    for (int i = 0; i < n; i++) p[o++] = bytes[i];
    return o;
}

// Send one DHCP message broadcast. requested_ip/server_id are used by
// REQUEST (RFC 2131 section 4.3.2); NULL for DISCOVER.
static void dhcp_send_msg(uint8_t msg_type, const uint8_t* requested_ip, const uint8_t* server_id) {
    uint8_t pkt[1500];
    memset(pkt, 0, sizeof(pkt));
    dhcp_packet_t* d = (dhcp_packet_t*)pkt;

    d->op    = 1; // BOOTREQUEST
    d->htype = 1;
    d->hlen  = 6;
    d->xid   = htonl(dhcp_xid);
    d->flags = htons(0x8000); // server must broadcast its reply
    memcpy(d->chaddr, rtl_mac, 6);
    d->magic[0] = 0x63; d->magic[1] = 0x82; d->magic[2] = 0x53; d->magic[3] = 0x63;

    int o = sizeof(dhcp_packet_t);
    o = dhcp_opt3(pkt, o, 53, msg_type);                    // message type
    if (msg_type == DHCP_REQUEST) {
        if (requested_ip) o = dhcp_optN(pkt, o, 50, requested_ip, 4);  // req. IP
        if (server_id)    o = dhcp_optN(pkt, o, 54, server_id, 4);     // server id
    } else {
        // Parameter request list: subnet mask (1), router (3), DNS (6)
        pkt[o++] = 55; pkt[o++] = 3; pkt[o++] = 1; pkt[o++] = 3; pkt[o++] = 6;
    }
    pkt[o++] = 255; // end option

    uint8_t bcast_mac[6]  = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t zero_ip[4]    = {0, 0, 0, 0};
    uint8_t bcast_ip[4]   = {255, 255, 255, 255};
    net_send_udp_raw(zero_ip, bcast_ip, bcast_mac, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, pkt, (uint32_t)o);
}

static void dhcp_send_discover(void) {
    write_serial_string("[DHCP] DISCOVER xid=");
    write_serial_hex(dhcp_xid);
    write_serial_string("\n");
    dhcp_send_msg(DHCP_DISCOVER, NULL, NULL);
}

static void dhcp_send_request(void) {
    write_serial_string("[DHCP] REQUEST xid=");
    write_serial_hex(dhcp_xid);
    write_serial_string("\n");
    dhcp_send_msg(DHCP_REQUEST, dhcp_offered_ip, dhcp_server_id);
}

// Apply an OFFER/ACK's option set to the running config (only non-zero
// entries override the static defaults).
static void dhcp_apply_config(const uint8_t* yiaddr) {
    memcpy(my_ip, yiaddr, 4);
    if (dhcp_router[0] || dhcp_router[1] || dhcp_router[2] || dhcp_router[3]) {
        memcpy(gateway_ip, dhcp_router, 4);
    }
    if (dhcp_dns[0] || dhcp_dns[1] || dhcp_dns[2] || dhcp_dns[3]) {
        memcpy(dns_server_ip, dhcp_dns, 4);
    }
    if (dhcp_mask[0] || dhcp_mask[1] || dhcp_mask[2] || dhcp_mask[3]) {
        memcpy(netmask_ip, dhcp_mask, 4);
    }
    dhcp_bound = 1;
    dhcp_state = DHCP_STATE_BOUND;
}

// Parse an incoming BOOTREPLY. Runs from the RX path (IRQ or net_poll, both
// with IF=0) — only touches globals and may send the REQUEST, exactly like
// the existing ARP-reply path.
static void dhcp_handle(uint8_t* data, uint32_t len) {
    if (len < sizeof(dhcp_packet_t)) return;
    dhcp_packet_t* d = (dhcp_packet_t*)data;
    if (d->op != 2) return;                        // BOOTREPLY
    if (d->magic[0] != 0x63 || d->magic[1] != 0x82 ||
        d->magic[2] != 0x53 || d->magic[3] != 0x63) return;
    if (ntohl(d->xid) != dhcp_xid) return;         // not our transaction
    if (memcmp(d->chaddr, rtl_mac, 6) != 0) return; // for another client

    // Walk the options.
    uint8_t msg_type = 0;
    int o = sizeof(dhcp_packet_t);
    while (o < (int)len && o < 1500) {
        uint8_t t = data[o++];
        if (t == 255) break;                       // END
        if (t == 0) continue;                      // PAD
        if (o >= (int)len) break;
        uint8_t n = data[o++];
        if (o + n > (int)len) break;
        if (t == 53 && n >= 1) msg_type = data[o];
        else if (t == 1 && n >= 4) memcpy(dhcp_mask, data + o, 4);
        else if (t == 3 && n >= 4) memcpy(dhcp_router, data + o, 4);
        else if (t == 6 && n >= 4) memcpy(dhcp_dns, data + o, 4);
        else if (t == 54 && n >= 4) memcpy(dhcp_server_id, data + o, 4);
        o += n;
    }

    write_serial_string("[DHCP] reply type=");
    write_serial_hex(msg_type);
    write_serial_string(" yiaddr=");
    write_serial_hex((d->yiaddr[0] << 24) | (d->yiaddr[1] << 16) | (d->yiaddr[2] << 8) | d->yiaddr[3]);
    write_serial_string("\n");

    if (msg_type == DHCP_OFFER && dhcp_state == DHCP_STATE_DISCOVER) {
        memcpy(dhcp_offered_ip, d->yiaddr, 4);
        dhcp_state = DHCP_STATE_REQUEST;
        dhcp_retries = 0;
        dhcp_last_send_tick = get_ticks();
        dhcp_send_request();
    } else if (msg_type == DHCP_ACK && dhcp_state == DHCP_STATE_REQUEST) {
        dhcp_apply_config(d->yiaddr);
        write_serial_string("[DHCP] ACK — bound ");
        write_serial_hex((my_ip[0] << 24) | (my_ip[1] << 16) | (my_ip[2] << 8) | my_ip[3]);
        write_serial_string(" gw=");
        write_serial_hex((gateway_ip[0] << 24) | (gateway_ip[1] << 16) | (gateway_ip[2] << 8) | gateway_ip[3]);
        write_serial_string(" dns=");
        write_serial_hex((dns_server_ip[0] << 24) | (dns_server_ip[1] << 16) | (dns_server_ip[2] << 8) | dns_server_ip[3]);
        write_serial_string("\n");
        // Resolve the (possibly new) gateway MAC — its ARP reply is what
        // flips net_ready and dispatches any queued DNS/TCP operations.
        net_send_arp_request(gateway_ip);
    } else if (msg_type == DHCP_NAK && dhcp_state == DHCP_STATE_REQUEST) {
        write_serial_string("[DHCP] NAK — restarting discovery\n");
        dhcp_state = DHCP_STATE_IDLE;
        dhcp_retries = 0;
    }
}

// Give up on DHCP and use the static defaults (also re-resolves the gateway
// MAC so networking comes up exactly as it did before DHCP existed).
static void dhcp_fallback_static(void) {
    write_serial_string("[DHCP] no server after ");
    write_serial_hex(DHCP_MAX_RETRIES + 1);
    write_serial_string(" tries — static config\n");
    dhcp_state = DHCP_STATE_BOUND;   // stop the retry loop
    net_send_arp_request(gateway_ip);
}

// Drive the client state machine. Called from net_poll() inside its cli
// window so the IRQ RX path can never interleave a parse with a retry.
static void dhcp_tick(void) {
    if (!rtl_present) return;
    if (dhcp_state == DHCP_STATE_IDLE) {
        dhcp_state = DHCP_STATE_DISCOVER;
        dhcp_retries = 0;
        dhcp_last_send_tick = get_ticks();
        dhcp_send_discover();
    } else if (dhcp_state == DHCP_STATE_DISCOVER || dhcp_state == DHCP_STATE_REQUEST) {
        if (get_ticks() - dhcp_last_send_tick >= DHCP_RETRY_MS) {
            dhcp_retries++;
            if (dhcp_retries > DHCP_MAX_RETRIES) {
                dhcp_fallback_static();
                return;
            }
            dhcp_last_send_tick = get_ticks();
            if (dhcp_state == DHCP_STATE_DISCOVER) dhcp_send_discover();
            else dhcp_send_request();
        }
    }
}

// Handle incoming TCP
static void net_handle_tcp(ip_header_t* ip, uint8_t* tcp_data, uint32_t tcp_len) {
    if (tcp_len < sizeof(tcp_header_t)) return;
    tcp_header_t* tcp = (tcp_header_t*)tcp_data;

    uint16_t src_port = ntohs(tcp->src_port);
    uint16_t dst_port = ntohs(tcp->dst_port);

    // Match the segment to a connection by the full 4-tuple
    tcp_conn_t* c = NULL;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (tcp_conns[i].in_use &&
            tcp_conns[i].local_port == dst_port &&
            tcp_conns[i].remote_port == src_port &&
            memcmp(tcp_conns[i].remote_ip, ip->src_ip, 4) == 0) {
            c = &tcp_conns[i];
            break;
        }
    }
    if (!c) {
        // No established connection matched. This is where a server accepts:
        // a SYN aimed at a TCP_LISTEN slot spawns a NEW child slot that runs
        // the passive handshake on its own — the listener itself stays in
        // TCP_LISTEN and keeps accepting (POSIX backlog model, v38.43).
        if (tcp->flags & TCP_SYN) {
            int listener = -1;
            for (int i = 0; i < TCP_MAX_CONNS; i++) {
                if (tcp_conns[i].in_use && tcp_conns[i].state == TCP_LISTEN &&
                    tcp_conns[i].local_port == dst_port) {
                    listener = i;
                    break;
                }
            }
            if (listener >= 0) {
                int child = -1;
                for (int i = 0; i < TCP_MAX_CONNS; i++) {
                    if (!tcp_conns[i].in_use) { child = i; break; }
                }
                if (child < 0) {
                    // Backlog exhausted (no free slot): drop the SYN. The
                    // peer retransmits and gets in once a slot frees up.
                    write_serial_string("[TCP] listen backlog full, SYN dropped\n");
                    return;
                }
                tcp_conn_t* ch = &tcp_conns[child];
                memset(ch, 0, sizeof(tcp_conn_t));
                ch->in_use = 1;
                ch->state = TCP_SYN_RCVD;
                ch->local_port = tcp_conns[listener].local_port;
                memcpy(ch->remote_ip, ip->src_ip, 4);
                ch->remote_port = src_port;
                ch->seq = get_ticks() * 54321 + dst_port;  // our ISN
                ch->ack = ntohl(tcp->seq) + 1;             // ACK the SYN
                ch->listen_parent = listener;
                ch->accepted = 0;
                ch->conn_start_tick = get_ticks();
                write_serial_string("[TCP] SYN on listening port ");
                write_serial_hex(dst_port);
                write_serial_string(" -> child conn ");
                write_serial_hex(child);
                write_serial_string("\n");
                net_send_tcp_segment(ch, TCP_SYN | TCP_ACK, 0, 0);
                ch->seq++;   // SYN consumes one sequence number
                return;
            }
        }
        write_serial_string("[TCP] segment for unknown connection, dropped\n");
        return;
    }

    // Validate the TCP checksum (pseudo-header + segment). A valid segment
    // re-checksums to zero; corrupted ones are dropped instead of polluting
    // the receive buffer.
    uint8_t pseudo_buf[1500];
    if (tcp_len > sizeof(pseudo_buf) - sizeof(tcp_pseudo_header_t)) return;
    tcp_pseudo_header_t* psh = (tcp_pseudo_header_t*)pseudo_buf;
    memcpy(psh->src_ip, ip->src_ip, 4);
    memcpy(psh->dst_ip, my_ip, 4);
    psh->zeros = 0;
    psh->protocol = IP_PROTO_TCP;
    psh->tcp_len = htons((uint16_t)tcp_len);
    memcpy(pseudo_buf + sizeof(tcp_pseudo_header_t), tcp, tcp_len);
    if (ip_checksum(pseudo_buf, sizeof(tcp_pseudo_header_t) + tcp_len) != 0) {
        write_serial_string("[TCP] bad checksum, dropped\n");
        return;
    }

    // Remote reset: kill the connection immediately
    if (tcp->flags & TCP_RST) {
        write_serial_string("[TCP] connection reset by peer (RST)\n");
        c->state = TCP_CLOSED;
        c->in_use = 0;
        return;
    }

    if (c->state == TCP_SYN_SENT) {
        if ((tcp->flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) &&
            ntohl(tcp->ack) == c->seq) {
            // Received SYN-ACK for our ISN
            c->ack = ntohl(tcp->seq) + 1;
            c->state = TCP_ESTABLISHED;
            c->retrans_count = 0;
            write_serial_string("[TCP] connection established\n");
            net_send_tcp_segment(c, TCP_ACK, 0, 0);
        }
        return;
    }

    // Passive open: the peer's final ACK of our SYN-ACK completes the
    // handshake. A duplicate SYN just re-sends the SYN-ACK (idempotent).
    if (c->state == TCP_SYN_RCVD) {
        if ((tcp->flags & TCP_ACK) && !(tcp->flags & TCP_SYN) &&
            ntohl(tcp->ack) == c->seq) {
            c->state = TCP_ESTABLISHED;
            c->retrans_count = 0;
            write_serial_string("[TCP] server: connection established (accepted)\n");
        } else if (tcp->flags & TCP_SYN) {
            net_send_tcp_segment(c, TCP_SYN | TCP_ACK, 0, 0);
            c->seq = ntohl(tcp->seq) + 1;   // re-ACK the retransmitted SYN
        }
        return;
    }

    uint8_t header_len = (tcp->data_offset_res >> 4) * 4;
    if (header_len < sizeof(tcp_header_t) || header_len > tcp_len) return;
    uint32_t payload_len = tcp_len - header_len;
    uint32_t seg_seq = ntohl(tcp->seq);
    int fin = tcp->flags & TCP_FIN;
    int ack_flag = tcp->flags & TCP_ACK;

    // ACK of our sent data clears the retransmit state
    if (ack_flag && c->unacked_len && ntohl(tcp->ack) >= c->unacked_seq + c->unacked_len) {
        c->unacked_len = 0;
        c->tx_pending_len = 0;
        c->retrans_count = 0;
    }

    // In-order data append (duplicates / out-of-order get a re-ACK, never data)
    if (payload_len > 0 && (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT)) {
        if (seg_seq == c->ack) {
            if (c->rx_len + (int)payload_len <= TCP_CONN_BUF) {
                memcpy(c->rx + c->rx_len, tcp_data + header_len, payload_len);
                c->rx_len += (int)payload_len;
            }
            c->ack = seg_seq + payload_len;
            net_send_tcp_segment(c, TCP_ACK, 0, 0);
        } else {
            net_send_tcp_segment(c, TCP_ACK, 0, 0); // stale/dup: refresh window
        }
    }

    // Our FIN was acknowledged — require the ACK to actually cover the FIN's
    // sequence number, not just be any ACK (a duplicate/stray ACK must not
    // advance the close handshake early). c->seq was bumped when the FIN was
    // sent, so FIN_WAIT_1 expects ack >= c->seq.
    if (c->state == TCP_FIN_WAIT_1 && ack_flag && ntohl(tcp->ack) >= c->seq) {
        if (fin) { // peer FIN came in the same segment: done
            c->state = TCP_CLOSED;
            c->in_use = 0;
            write_serial_string("[TCP] closed (FIN|ACK)\n");
            return;
        }
        c->state = TCP_FIN_WAIT_2;
    } else if (c->state == TCP_LAST_ACK && ack_flag && ntohl(tcp->ack) >= c->seq) {
        c->state = TCP_CLOSED;
        c->in_use = 0;
        write_serial_string("[TCP] closed (final ACK)\n");
        return;
    }

    // Peer FIN: half-close from the remote side
    if (fin && c->in_use) {
        if (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) {
            if (c->state == TCP_ESTABLISHED) {
                c->state = TCP_CLOSE_WAIT;
                write_serial_string("[TCP] peer closed connection (CLOSE_WAIT)\n");
            }
            c->ack = seg_seq + payload_len + 1;
            c->eof = 1;
            net_send_tcp_segment(c, TCP_ACK, 0, 0);
        } else if (c->state == TCP_FIN_WAIT_2) {
            c->ack = seg_seq + payload_len + 1;
            net_send_tcp_segment(c, TCP_ACK, 0, 0);
            c->state = TCP_CLOSED;
            c->in_use = 0;
            write_serial_string("[TCP] closed after peer FIN\n");
        }
    }
}

// Handle UDP
static void net_handle_udp(ip_header_t* ip, uint8_t* udp_data, uint32_t udp_len) {
    (void)ip;
    if (udp_len < sizeof(udp_header_t)) return;
    udp_header_t* udp = (udp_header_t*)udp_data;
    
    uint16_t src_port = ntohs(udp->src_port);
    uint16_t dst_port = ntohs(udp->dst_port);
    write_serial_string("[NET] net_handle_udp: src_port=");
    write_serial_hex(src_port);
    write_serial_string(" dst_port=");
    write_serial_hex(dst_port);
    write_serial_string("\n");
    
    uint16_t udp_frame_len = ntohs(udp->len);
    if (udp_frame_len < sizeof(udp_header_t) || udp_frame_len > udp_len) return;
    uint32_t payload_len = udp_frame_len - sizeof(udp_header_t);
    
    if (src_port == 53) {
        net_handle_dns(udp_data + sizeof(udp_header_t), payload_len);
        return;
    }

    // DHCP replies (OFFER/ACK/NAK) land on our client port 68. Ignored once
    // bound — the xid/chaddr checks in dhcp_handle keep strays out regardless.
    if (dst_port == DHCP_CLIENT_PORT && dhcp_state != DHCP_STATE_BOUND) {
        dhcp_handle(udp_data + sizeof(udp_header_t), payload_len);
        return;
    }

    // Application datagram: deliver to the bound UDP socket, if any.
    // NOTE: no cli/sti here — this runs inside net_irq_handler (IF already
    // cleared by the interrupt gate) or net_poll (which we wrap in cli/sti).
    // Re-enabling IF mid-IRQ would allow nested re-entrant net processing.
    if (dst_port == udp_bind_port && udp_bind_port != 0) {
        if ((int)payload_len <= UDP_RX_BUF_SIZE) {
            memcpy(udp_rx_buf, udp_data + sizeof(udp_header_t), payload_len);
            udp_rx_len = (int)payload_len;
            memcpy(udp_last_src_ip, ip->src_ip, 4);
            udp_last_src_port = src_port;
        }
        write_serial_string("[NET] UDP datagram queued for app (port ");
        write_serial_hex(dst_port);
        write_serial_string(")\n");
    }
}


// Handle incoming IP packet
static void net_handle_ip(uint8_t* data, uint32_t len) {
    if (len < sizeof(ip_header_t)) return;
    ip_header_t* ip = (ip_header_t*)data;
    
    write_serial_string("[NET] net_handle_ip: protocol=");
    write_serial_hex(ip->protocol);
    write_serial_string(" dst_ip=");
    write_serial_hex((ip->dst_ip[0] << 24) | (ip->dst_ip[1] << 16) | (ip->dst_ip[2] << 8) | ip->dst_ip[3]);
    write_serial_string("\n");

    uint32_t ihl = (ip->ver_ihl & 0x0F) * 4;
    uint32_t total = ntohs(ip->total_len);
    if (total > len) return;
    // IHL < 20 is malformed and IHL > total means total - ihl underflows,
    // which previously fed a ~4GB memcpy in net_handle_icmp.
    if (ihl < sizeof(ip_header_t) || ihl > total) return;

    // Check if it's for us. Before DHCP binds us we have no address, so the
    // server's OFFER/ACK can arrive as a broadcast (255.255.255.255 — we set
    // the broadcast flag) or to 0.0.0.0; accept those while unbound only.
    if (ip->dst_ip[0] != my_ip[0] || ip->dst_ip[1] != my_ip[1] ||
        ip->dst_ip[2] != my_ip[2] || ip->dst_ip[3] != my_ip[3]) {
        int unbound = dhcp_state != DHCP_STATE_BOUND;
        int bcast = (ip->dst_ip[0] == 255 && ip->dst_ip[1] == 255 &&
                     ip->dst_ip[2] == 255 && ip->dst_ip[3] == 255);
        int zero  = (ip->dst_ip[0] == 0 && ip->dst_ip[1] == 0 &&
                     ip->dst_ip[2] == 0 && ip->dst_ip[3] == 0);
        if (!unbound || (!bcast && !zero)) {
            write_serial_string("[NET] net_handle_ip: discarded because dst_ip is not for us\n");
            return;
        }
    }


    if (ip->protocol == IP_PROTO_ICMP) {
        net_handle_icmp(ip, data + ihl, total - ihl);
    } else if (ip->protocol == IP_PROTO_UDP) {
        net_handle_udp(ip, data + ihl, total - ihl);
    } else if (ip->protocol == IP_PROTO_TCP) {
        net_handle_tcp(ip, data + ihl, total - ihl);
    }
}

// Process one incoming frame
static void net_handle_frame(uint8_t* frame, uint32_t len) {
    if (len < sizeof(eth_header_t)) return;
    eth_header_t* eth = (eth_header_t*)frame;
    uint16_t type = ntohs(eth->ethertype);
    uint8_t* payload = frame + sizeof(eth_header_t);
    uint32_t payload_len = len - sizeof(eth_header_t);
    
    write_serial_string("[NET] net_handle_frame: received packet type=");
    write_serial_hex(type);
    write_serial_string("\n");


    if (type == ETH_TYPE_ARP) {
        if (payload_len >= sizeof(arp_packet_t)) {
            net_handle_arp((arp_packet_t*)payload);
        }
    } else if (type == ETH_TYPE_IP) {
        net_handle_ip(payload, payload_len);
    }
}

// Initialize networking: start the DHCP client. dhcp_tick() (driven from
// net_poll) sends the first DISCOVER; if no server answers, the static
// defaults take over exactly as before (ARP -> net_ready).
void net_init(void) {
    if (!rtl_present) return;
    write_serial_string("[NET] init: starting DHCP client\n");
    dhcp_xid = (uint32_t)get_ticks() * 2654435761u + 0x4D4354u; // odd-ish xid
    dhcp_state = DHCP_STATE_IDLE;
    dhcp_tick();
}

// Poll for incoming packets (call from main loop — fallback for the shell's
// busy-waits; the IRQ path is the primary driver now).
// Interrupts are disabled around the drain so the IRQ handler (same CPU) can
// never interleave its rtl_rx_offset/CAPR updates with ours.
void net_poll(void) {
    if (!rtl_present) return;
    uint8_t buf[1520];
    int len;
    __asm__ volatile("cli");
    // Process up to 4 packets per poll
    for (int i = 0; i < 4; i++) {
        len = rtl8139_poll_rx(buf, sizeof(buf));
        if (len <= 0) break;
        write_serial_string("[NET] rtl8139_poll_rx: packet received, len=");
        write_serial_hex(len);
        write_serial_string("\n");
        net_handle_frame(buf, (uint32_t)len);
    }

    // DHCP state machine (retries/fallback). Same cli window: a DISCOVER/REQUEST
    // retry must not race the IRQ path parsing an OFFER/ACK.
    dhcp_tick();

    // TCP retransmit + connect timeout sweep. Runs inside the same cli/sti
    // window as the RX drain so the IRQ path can never interleave.
    uint32_t now = get_ticks();
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        tcp_conn_t* c = &tcp_conns[i];
        if (!c->in_use) continue;

        if (c->state == TCP_SYN_SENT) {
            if (now - c->conn_start_tick >= TCP_CONNECT_TIMEOUT_MS) {
                write_serial_string("[TCP] connect timeout, aborting\n");
                c->state = TCP_CLOSED;
                c->in_use = 0;
            }
            // No SYN retransmit: on the local link a lost SYN is vanishingly
            // rare, and a duplicate SYN after the peer's SYN-ACK is already
            // queued poisons the connection with a spurious RST.
        } else if (c->unacked_len > 0 &&
                   c->retrans_count < TCP_RETRANS_MAX &&
                   now - c->last_tx_tick >= TCP_RETRANS_MS) {
            c->retrans_count++;
            uint32_t saved = c->seq;
            c->seq = c->unacked_seq; // resend the unacked data segment
            net_send_tcp_segment(c, TCP_ACK | TCP_PSH, c->tx_pending, c->tx_pending_len);
            c->seq = saved;
        }
    }
    __asm__ volatile("sti");
}

// IRQ-driven RX: called from the IRQ 11 handler. Interrupts are already
// disabled, so no re-entrancy against net_poll() is possible.
void net_irq_handler(registers_t* r) {
    (void)r;
    if (!rtl_present) return;
    uint16_t isr = rtl8139_irq_clear();
    if (isr & 0x01) { // ROK — packets available
        uint8_t buf[1520];
        int len;
        for (int i = 0; i < 8; i++) { // drain up to 8 frames per IRQ
            len = rtl8139_poll_rx(buf, sizeof(buf));
            if (len <= 0) break;
            write_serial_string("[NET] IRQ rx, len=");
            write_serial_hex(len);
            write_serial_string("\n");
            net_handle_frame(buf, (uint32_t)len);
        }
    }
}


// Send ICMP Echo Request (ping)
void net_send_ping(uint8_t* target_ip) {
    if (!rtl_present) return;

    // If we don't know gateway MAC yet, send another ARP
    if (!net_ready) {
        net_send_arp_request(gateway_ip);
        return;
    }

    uint8_t pkt[64];
    ip_header_t* ip = (ip_header_t*)pkt;
    icmp_header_t* icmp = (icmp_header_t*)(pkt + sizeof(ip_header_t));

    uint32_t icmp_len = sizeof(icmp_header_t) + 8; // 8 bytes of data
    uint32_t total = sizeof(ip_header_t) + icmp_len;

    // IP header
    ip->ver_ihl    = 0x45;
    ip->tos        = 0;
    ip->total_len  = htons(total);
    ip->id         = htons(ping_seq);
    ip->flags_frag = 0;
    ip->ttl        = 64;
    ip->protocol   = IP_PROTO_ICMP;
    ip->checksum   = 0;
    memcpy(ip->src_ip, my_ip, 4);
    memcpy(ip->dst_ip, target_ip, 4);
    ip->checksum   = ip_checksum(ip, sizeof(ip_header_t));

    // ICMP Echo Request
    icmp->type     = ICMP_ECHO_REQUEST;
    icmp->code     = 0;
    icmp->id       = htons(0xBEEF);
    icmp->seq      = htons(ping_seq++);
    icmp->checksum = 0;
    // Fill 8 bytes of data after header
    memset(pkt + sizeof(ip_header_t) + sizeof(icmp_header_t), 0xAB, 8);
    icmp->checksum = ip_checksum(icmp, icmp_len);

    ping_replied = 0;
    ping_sent_tick = get_ticks();

    net_send_eth(gateway_mac, ETH_TYPE_IP, pkt, total);
}
