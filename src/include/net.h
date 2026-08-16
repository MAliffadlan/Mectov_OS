#ifndef NET_H
#define NET_H

#include "types.h"
#include "idt.h"  // registers_t (for net_irq_handler)

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

// TCP connection states
enum {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_LISTEN,   // passive open: waiting for a SYN (server side)
    TCP_SYN_RCVD  // SYN received, SYN-ACK sent, waiting for the final ACK
};

#define TCP_MAX_CONNS 16
#define TCP_CONN_BUF  16384

typedef struct {
    int  in_use;                 // slot allocated
    int  state;
    uint8_t  remote_ip[4];
    uint16_t remote_port;
    uint16_t local_port;
    uint32_t seq;                // next sequence number to send
    uint32_t ack;                // next sequence number expected from peer
    uint8_t  rx[TCP_CONN_BUF];   // received data, consumed by the app
    int  rx_len;
    int  eof;                    // peer sent FIN (recv returns -1 after drain)
    uint32_t last_tx_tick;       // last time a segment was sent (retransmit timer)
    uint32_t unacked_seq;        // sequence of the oldest unacked data segment
    uint32_t unacked_len;        // payload length of that segment (0 = none)
    uint8_t  tx_pending[1400];   // copy of the unacked data segment
    int      tx_pending_len;
    int  retrans_count;
    uint32_t conn_start_tick;    // when the connect attempt began
    // Server accept model (v38.43): a LISTEN slot never mutates into a
    // connection. An inbound SYN spawns a NEW slot whose listen_parent is
    // the listener's id; net_tcp_accept hands ESTABLISHED children to the
    // app (one call per child) and marks them accepted. -1 = not a child.
    int  listen_parent;
    int  accepted;
} tcp_conn_t;

// Returns the connection id (0..TCP_MAX_CONNS-1) or -1 if no free slot.
int  net_tcp_connect(uint8_t* target_ip, uint16_t port);
// Passive open: reserve a slot and wait for inbound connections on `port`.
// The slot STAYS in TCP_LISTEN; each inbound SYN spawns a fresh child slot
// (net_handle_tcp) that completes the handshake on its own. Server apps
// poll net_tcp_accept(listener) for established children, POSIX-style.
int  net_tcp_listen(uint16_t port);
// Server accept (v38.43): return the id of one ESTABLISHED child spawned by
// this listener and mark it handed-out, or -1 when no completed child is
// pending. SYN_RCVD children still completing the handshake are skipped —
// the free-slot count IS the backlog.
int  net_tcp_accept(int listener_id);
// Non-destructive check used by poll(): 1 when accept() would return a conn.
int  net_tcp_accept_pending(int listener_id);
int  net_tcp_send(int id, uint8_t* payload, uint32_t len);
// Returns bytes copied, 0 = nothing new, -1 = connection closed/EOF, -2 = bad id.
int  net_tcp_recv(int id, uint8_t* out, uint32_t max_len);
void net_tcp_close(int id);
// State of one connection by id (TCP_CLOSED if the id is invalid).
int  net_tcp_state(int id);
// Poll helpers for the fd layer (v38.43): 1 when unread bytes are buffered /
// when the peer closed its write side (EOF after drain).
int  net_tcp_rx_pending(int id);
int  net_tcp_eof(int id);
// State of the most recently created connection (for SYS_NET_STATUS compat).
int  net_tcp_latest_state(void);


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

// Our network config. Static defaults (QEMU slirp) until a DHCP ACK
// overwrites them at runtime (see dhcp_* below).
extern uint8_t my_ip[4];
extern uint8_t gateway_ip[4];
extern uint8_t gateway_mac[6];
extern uint8_t dns_server_ip[4];
extern uint8_t netmask_ip[4];
extern int     net_ready;
extern int     ping_replied;
extern uint32_t ping_rtt;

// DHCP client state: 1 once a DHCP ACK bound us (vs. static fallback).
extern int     dhcp_bound;

// DHCP ports (RFC 2131)
#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

// DHCP message types (option 53)
#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

extern int     dns_resolved;
extern uint8_t dns_resolved_ip[4];

// Network functions
void net_init(void);
void net_poll(void);
void net_send_ping(uint8_t* target_ip);
void net_send_arp_request(uint8_t* target_ip_addr);
void net_send_dns_query(const char* domain);

// IRQ-driven RX (called from IRQ 11 handler)
void net_irq_handler(registers_t* r);

// UDP API (Ring 3 accessible)
int net_udp_bind(uint16_t port);
int net_udp_send(uint8_t* target_ip, uint16_t dst_port, void* payload, uint32_t payload_len);
int net_udp_recv(uint8_t* out, uint32_t max_len);
void net_udp_peer(uint8_t* ip_out, uint16_t* port_out);

// Utility
uint16_t htons(uint16_t val);
uint16_t ntohs(uint16_t val);

#endif
