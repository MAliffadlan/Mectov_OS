#include "../include/net.h"
#include "../include/rtl8139.h"
#include "../include/utils.h"
#include "../include/timer.h"

// Our IP: 10.0.2.15 (QEMU default guest IP with -net user)
uint8_t my_ip[4]       = {10, 0, 2, 15};
// Gateway: 10.0.2.2 (QEMU default gateway)
uint8_t gateway_ip[4]  = {10, 0, 2, 2};
uint8_t gateway_mac[6] = {0, 0, 0, 0, 0, 0};
int     net_ready       = 0;
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

// TCP Globals
int tcp_state = 0; // TCP_CLOSED
uint8_t tcp_target_ip[4] = {0};
uint16_t tcp_local_port = 54321;
uint16_t tcp_remote_port = 80;
uint32_t tcp_seq_num = 0x11223344;
uint32_t tcp_ack_num = 0;

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
            memcpy(gateway_mac, arp->sender_mac, 6);
            net_ready = 1;
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

// TCP segment send helper
static void net_send_tcp_segment(uint8_t flags, uint8_t* payload, uint32_t payload_len) {
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
    memcpy(ip->dst_ip, tcp_target_ip, 4);
    ip->checksum   = ip_checksum(ip, sizeof(ip_header_t));

    tcp->src_port = htons(tcp_local_port);
    tcp->dst_port = htons(tcp_remote_port);
    tcp->seq      = htonl(tcp_seq_num);
    tcp->ack      = htonl(tcp_ack_num);
    tcp->data_offset_res = (sizeof(tcp_header_t) / 4) << 4;
    tcp->flags    = flags;
    tcp->window_size = htons(8192);
    tcp->checksum = 0;
    tcp->urgent_ptr = 0;

    if (payload_len > 0) {
        memcpy(pkt + sizeof(ip_header_t) + sizeof(tcp_header_t), payload, payload_len);
    }

    // Compute TCP checksum with pseudo-header
    uint8_t pseudo_buf[1500];
    tcp_pseudo_header_t* psh = (tcp_pseudo_header_t*)pseudo_buf;
    memcpy(psh->src_ip, my_ip, 4);
    memcpy(psh->dst_ip, tcp_target_ip, 4);
    psh->zeros = 0;
    psh->protocol = IP_PROTO_TCP;
    psh->tcp_len = htons(tcp_len);
    
    memcpy(pseudo_buf + sizeof(tcp_pseudo_header_t), tcp, tcp_len);
    tcp->checksum = ip_checksum(pseudo_buf, sizeof(tcp_pseudo_header_t) + tcp_len);
    // Write back computed checksum
    memcpy(pkt + sizeof(ip_header_t), tcp, sizeof(tcp_header_t));

    net_send_eth(gateway_mac, ETH_TYPE_IP, pkt, total);
}

void net_tcp_connect(uint8_t* target_ip, uint16_t port) {
    if (!net_ready) return;
    memcpy(tcp_target_ip, target_ip, 4);
    tcp_remote_port = port;
    tcp_seq_num = get_ticks() * 12345; // random ISN
    tcp_ack_num = 0;
    tcp_state = TCP_SYN_SENT;
    
    net_send_tcp_segment(TCP_SYN, 0, 0);
    tcp_seq_num++; // SYN consumes one sequence number
}

void net_tcp_send(uint8_t* payload, uint32_t len) {
    if (tcp_state != TCP_ESTABLISHED) return;
    net_send_tcp_segment(TCP_ACK | TCP_PSH, payload, len);
    tcp_seq_num += len;
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
        net_send_arp_request(gateway_ip);
        return;
    }
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

    // Use Google DNS
    uint8_t dns_server[4] = {8, 8, 8, 8};
    net_send_udp(dns_server, 12345, 53, payload, qpos);
}

// Handle DNS Reply
static void net_handle_dns(uint8_t* data, uint32_t len) {
    if (len < sizeof(dns_header_t)) return;
    dns_header_t* dns = (dns_header_t*)data;
    if (ntohs(dns->id) != 0xABCD) return;
    
    // Check if it's a response (QR bit)
    if (!(ntohs(dns->flags) & 0x8000)) return;

    int qdcount = ntohs(dns->qdcount);
    int ancount = ntohs(dns->ancount);
    if (ancount == 0) return; // No answers

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

    // Parse first answer
    if (pos < (int)len) {
        // Skip name (usually a pointer)
        if ((data[pos] & 0xC0) == 0xC0) pos += 2;
        else {
            while (pos < (int)len && data[pos] != 0) pos += data[pos] + 1;
            pos++;
        }
        if (pos + 10 <= (int)len) {
            uint16_t type = (data[pos] << 8) | data[pos+1];
            pos += 8; // type(2), class(2), ttl(4)
            uint16_t rdlength = (data[pos] << 8) | data[pos+1];
            pos += 2;
            if (type == 1 && rdlength == 4 && pos + 4 <= (int)len) {
                // A Record found!
                dns_resolved_ip[0] = data[pos];
                dns_resolved_ip[1] = data[pos+1];
                dns_resolved_ip[2] = data[pos+2];
                dns_resolved_ip[3] = data[pos+3];
                dns_resolved = 1;
            }
        }
    }
}

char tcp_rx_buf[4096];
int tcp_rx_len = 0;

// Handle incoming TCP
static void net_handle_tcp(ip_header_t* ip, uint8_t* tcp_data, uint32_t tcp_len) {
    (void)ip;
    if (tcp_len < sizeof(tcp_header_t)) return;
    tcp_header_t* tcp = (tcp_header_t*)tcp_data;

    uint16_t src_port = ntohs(tcp->src_port);
    uint16_t dst_port = ntohs(tcp->dst_port);

    if (dst_port != tcp_local_port || src_port != tcp_remote_port) return;

    if (tcp_state == TCP_SYN_SENT) {
        if ((tcp->flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            // Received SYN-ACK!
            tcp_ack_num = ntohl(tcp->seq) + 1;
            tcp_state = TCP_ESTABLISHED;
            // Send ACK
            net_send_tcp_segment(TCP_ACK, 0, 0);
        }
    } else if (tcp_state == TCP_ESTABLISHED) {
        // Handle incoming data if any
        uint8_t header_len = (tcp->data_offset_res >> 4) * 4;
        uint32_t payload_len = tcp_len - header_len;
        if (payload_len > 0) {
            // Append to tcp_rx_buf
            if (tcp_rx_len + payload_len < sizeof(tcp_rx_buf)) {
                memcpy(tcp_rx_buf + tcp_rx_len, tcp_data + header_len, payload_len);
                tcp_rx_len += payload_len;
            }
            // Send ACK
            tcp_ack_num = ntohl(tcp->seq) + payload_len;
            net_send_tcp_segment(TCP_ACK, 0, 0);
        }
    }
}

// Handle UDP
static void net_handle_udp(ip_header_t* ip, uint8_t* udp_data, uint32_t udp_len) {
    (void)ip;
    if (udp_len < sizeof(udp_header_t)) return;
    udp_header_t* udp = (udp_header_t*)udp_data;
    
    uint16_t src_port = ntohs(udp->src_port);
    uint32_t payload_len = ntohs(udp->len) - sizeof(udp_header_t);
    
    if (src_port == 53) {
        net_handle_dns(udp_data + sizeof(udp_header_t), payload_len);
    }
}

// Handle incoming IP packet
static void net_handle_ip(uint8_t* data, uint32_t len) {
    if (len < sizeof(ip_header_t)) return;
    ip_header_t* ip = (ip_header_t*)data;

    uint32_t ihl = (ip->ver_ihl & 0x0F) * 4;
    uint32_t total = ntohs(ip->total_len);
    if (total > len) return;

    // Check if it's for us
    if (ip->dst_ip[0] != my_ip[0] || ip->dst_ip[1] != my_ip[1] ||
        ip->dst_ip[2] != my_ip[2] || ip->dst_ip[3] != my_ip[3]) {
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

// Poll for incoming packets (call from main loop)
void net_poll(void) {
    if (!rtl_present) return;
    uint8_t buf[1520];
    int len;
    // Process up to 4 packets per poll
    for (int i = 0; i < 4; i++) {
        len = rtl8139_poll_rx(buf, sizeof(buf));
        if (len <= 0) break;
        net_handle_frame(buf, (uint32_t)len);
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
