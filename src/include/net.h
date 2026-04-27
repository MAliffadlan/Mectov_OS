#ifndef NET_H
#define NET_H

#include "types.h"

// Ethernet frame header (14 bytes)
typedef struct __attribute__((packed)) {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype;
} eth_header_t;

#define ETH_TYPE_ARP  0x0806
#define ETH_TYPE_IP   0x0800

// ARP packet (28 bytes for IPv4-over-Ethernet)
typedef struct __attribute__((packed)) {
    uint16_t hw_type;       // 1 = Ethernet
    uint16_t proto_type;    // 0x0800 = IPv4
    uint8_t  hw_len;        // 6
    uint8_t  proto_len;     // 4
    uint16_t opcode;        // 1=request, 2=reply
    uint8_t  sender_mac[6];
    uint8_t  sender_ip[4];
    uint8_t  target_mac[6];
    uint8_t  target_ip[4];
} arp_packet_t;

#define ARP_REQUEST 1
#define ARP_REPLY   2

// IPv4 header (20 bytes, no options)
typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;       // version(4) + IHL(4)
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint8_t  src_ip[4];
    uint8_t  dst_ip[4];
} ip_header_t;

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

// TCP header (20 bytes, no options)
typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_offset_res; // 4 bits data offset, 4 bits reserved
    uint8_t  flags;           // CWR, ECE, URG, ACK, PSH, RST, SYN, FIN
    uint16_t window_size;
    uint16_t checksum;
    uint16_t urgent_ptr;
} tcp_header_t;

// TCP Flags
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

// Simple TCP State Machine (Single Socket for now)
enum {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED
};

extern int tcp_state;
extern uint8_t tcp_target_ip[4];
extern uint16_t tcp_local_port;
extern uint16_t tcp_remote_port;
extern uint32_t tcp_seq_num;
extern uint32_t tcp_ack_num;

extern char tcp_rx_buf[4096];
extern int tcp_rx_len;

void net_tcp_connect(uint8_t* target_ip, uint16_t port);
void net_tcp_send(uint8_t* payload, uint32_t len);


// UDP header (8 bytes)
typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    uint16_t checksum;
} udp_header_t;

// DNS header (12 bytes)
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

// ICMP header
typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_header_t;

#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY   0

// Our network config
extern uint8_t my_ip[4];
extern uint8_t gateway_ip[4];
extern uint8_t gateway_mac[6];
extern int     net_ready;
extern int     ping_replied;
extern uint32_t ping_rtt;

extern int     dns_resolved;
extern uint8_t dns_resolved_ip[4];

// Network functions
void net_init(void);
void net_poll(void);
void net_send_ping(uint8_t* target_ip);
void net_send_arp_request(uint8_t* target_ip_addr);
void net_send_dns_query(const char* domain);

// Utility
uint16_t htons(uint16_t val);
uint16_t ntohs(uint16_t val);

#endif
