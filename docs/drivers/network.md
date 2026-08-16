# RTL8139 Network Stack & Web Gateway Proxy

Mectov OS includes a built-in Ethernet network stack (`src/drivers/net.c` & `src/drivers/rtl8139.c`) capable of real-time internet connectivity via QEMU user-mode networking.

---

## 🌐 Network Stack Layers

```
+--------------------------------------------------------------------+
|                Mectov Browser App / Network Syscalls               |
+--------------------------------------------------------------------+
|  TCP Stream Engine  |  UDP Transport Engine  |  DNS Resolver       |
+--------------------------------------------------------------------+
|  IPv4 Packet Layer  |  ICMP Echo (Ping)      |  ARP Resolution     |
+--------------------------------------------------------------------+
|  Ethernet Frame Layer (IEEE 802.3)                                 |
+--------------------------------------------------------------------+
|  RTL8139 PCI Driver (Bus Master DMA & Polling RX)                  |
+--------------------------------------------------------------------+
```

---

## 🔧 Hardware Driver & Network Parameters

1. **RTL8139 NIC (`src/drivers/rtl8139.c`)**:
   - Scans PCI bus for Vendor ID `0x10EC` & Device ID `0x8139`.
   - Enables PCI Bus Master & I/O Space bits.
   - Configures 8KB + 16-byte ring buffer for packet reception.

2. **Network Address Defaults (QEMU Slirp)**:
   - **Guest IP**: `10.0.2.15`
   - **Gateway IP**: `10.0.2.2`
   - **DNS Server**: `10.0.2.3`

## 📡 DHCP Client (`src/drivers/net.c`)

On boot the kernel runs a DHCP client (RFC 2131) instead of trusting the
static defaults: DISCOVER → OFFER → REQUEST → ACK, all over UDP broadcast.

- **Pre-config sends**: DISCOVER/REQUEST go out as `0.0.0.0:68 →
  255.255.255.255:67` to Ethernet `FF:FF:FF:FF:FF:FF` via `net_send_udp_raw()`
  — a raw UDP path with no `net_ready` gate (normal sends are gated on the
  gateway MAC being resolved).
- **Pre-config RX**: `net_handle_ip()` accepts broadcast (`255.255.255.255`)
  and `0.0.0.0` destinations while unbound, so OFFER/ACK frames get in; the
  client sets the broadcast flag (0x8000) to force broadcast replies.
- **Runtime update on ACK**: `my_ip`, `gateway_ip`, `dns_server_ip` (option 6)
  and `netmask_ip` (option 1) are overwritten in place, then the gateway MAC
  is re-resolved via ARP — that ARP reply is what flips `net_ready` and
  dispatches any queued DNS/TCP operations. The DNS query path now targets
  `dns_server_ip` instead of a hardcoded address.
- **State machine**: driven from `net_poll()` inside its cli window (retries
  every 1 s, 3 retries per phase); the IRQ RX path only parses and records
  OFFER/ACK. No server → static fallback after ~4 s, identical to the old
  behavior.
- **`ipconfig` shell command** prints the runtime config (IP/netmask/gateway/
  DNS, DHCP vs static, link state).

3. **Web Gateway Proxy Integration (`gateway.py`)**:
   - Outgoing HTTP connections on TCP port 80 are redirected by the network driver to `10.0.2.2:8888` (Host Web Gateway Proxy).
   - The python proxy fetches modern HTTPS web content, strips unnecessary bloat, and returns clean HTML/text readable by the Mectov OS Browser app.

---

## 🧵 Listener/Backlog Accept Model & POSIX Sockets (v38.43)

Before v38.43 a SYN aimed at a listening slot **mutated the listener
itself** into the connection (one pending connection per slot, no backlog).
The slot model is now POSIX-shaped:

- `tcp_conn_t` gained `listen_parent` + `accepted`. An inbound SYN finds
  the LISTEN slot for the port, allocates a **fresh child slot**, and runs
  SYN-RCVD → ESTABLISHED on the child; the listener stays listening and
  keeps accepting. Free slots ARE the backlog (SYN dropped + logged when
  exhausted; the peer's retransmit gets in once a slot frees).
- `net_tcp_accept(listener)` hands out one ESTABLISHED, not-yet-accepted
  child per call (SYS_ACCEPT attaches it to a new socket fd);
  `net_tcp_accept_pending` backs poll-POLLIN on listening sockets.
- Connection capacity is 16 slots (v38.44; was 8).
- The fd layer gained `FD_TYPE_SOCKET`: `sock_socket/bind/get_conn/
  set_conn/accept` in fd.c manage the descriptors; `do_sys_read`/`write`,
  `fd_poll_events` (POLLOUT = established, POLLIN = data/EOF/acceptable)
  and `fd_release_global` (close tears the conn down) are socket-aware.
