#include "../include/net.h"
#include "../include/rtl8139.h"
#include "../include/utils.h"
#include "../include/timer.h"
#include "../include/serial.h"


// Our IP: 10.0.2.15 (QEMU default guest IP with -net user)
uint8_t my_ip[4]       = {10, 0, 2, 15};
// Gateway: 10.0.2.2 (QEMU default gateway)
uint8_t gateway_ip[4]  = {10, 0, 2, 2};
uint8_t gateway_mac[6] = {0, 0, 0, 0, 0, 0};
int     net_ready       = 0;
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

// UDP send
static void net_send_udp(uint8_t* target_ip, uint16_t src_port, uint16_t dst_port, void* payload, uint32_t payload_len) {
    if (!net_ready) return;

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
    memcpy(ip->src_ip, my_ip, 4);
    memcpy(ip->dst_ip, target_ip, 4);
    ip->checksum   = ip_checksum(ip, sizeof(ip_header_t));

    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->len      = htons(udp_len);
    udp->checksum = 0; // Optional in IPv4

    memcpy(pkt + sizeof(ip_header_t) + sizeof(udp_header_t), payload, payload_len);

    net_send_eth(gateway_mac, ETH_TYPE_IP, pkt, total);
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
    c->seq = get_ticks() * 12345 + c->local_port; // random-ish ISN per connection
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

    // Use QEMU virtual DNS server (10.0.2.3) which proxies to host resolver
    uint8_t dns_server[4] = {10, 0, 2, 3};
    net_send_udp(dns_server, 12345, 53, payload, qpos);
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

    // Check if it's for us
    if (ip->dst_ip[0] != my_ip[0] || ip->dst_ip[1] != my_ip[1] ||
        ip->dst_ip[2] != my_ip[2] || ip->dst_ip[3] != my_ip[3]) {
        write_serial_string("[NET] net_handle_ip: discarded because dst_ip is not for us\n");
        return;
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

// Initialize networking: send ARP request to find gateway
void net_init(void) {
    if (!rtl_present) return;
    net_send_arp_request(gateway_ip);
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
