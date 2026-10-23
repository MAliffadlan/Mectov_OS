# Network Stack & Web Gateway Proxy

Mectov OS includes a built-in Ethernet network stack (`src/drivers/net.c`) capable of real-time internet connectivity via QEMU user-mode networking. The protocol layer is NIC-agnostic: since v38.169 it dispatches through a `net_backend_t` vtable (`src/include/net.h`) so a driver only supplies `send` / `poll_rx` / `irq_clear` / `get_mac`.

Three backends exist:

| Backend | File | Transport | RX |
|---|---|---|---|
| `rtl8139` | `src/drivers/rtl8139.c` | PCI, I/O ports, 8KB ring | IRQ 11 |
| `e1000e` | `src/drivers/e1000e.c` | PCI, MMIO, descriptor rings | IRQ |
| `virtio-net` | `src/drivers/virtio_net.c` | PCI legacy virtio (I/O BAR), two virtqueues | poll-only |

Priority (`net_backend_priority()` in `src/drivers/net.c`): virtio-net 3 > e1000e 2 > rtl8139 1. Paravirtual costs no emulation, so a hypervisor that offers it should be used; first-wins among equal ranks.

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
|  net_backend_t vtable (rtl8139 | e1000e | virtio-net)              |
+--------------------------------------------------------------------+
```

---

## 🔧 Hardware Driver & Network Parameters

1. **RTL8139 NIC (`src/drivers/rtl8139.c`)**:
   - Scans PCI bus for Vendor ID `0x10EC` & Device ID `0x8139`.
   - Enables PCI Bus Master & I/O Space bits.
   - Configures 8KB + 16-byte ring buffer for packet reception, plus a
     1500-byte linear overflow area: QEMU's 8139 model does not split a
     frame across the ring end when WRAP is set — it writes straddling
     frames linearly past 8K (v38.79; reading the tail from ring offset 0
     served stale bytes and stalled bulk TCP transfers).

2. **Intel e1000e NIC (`src/drivers/e1000e.c`)**, v38.170:
   - PCI `8086:100E` (82540EM) / `8086:10D3` (82574) / `8086:1533`.
   - MMIO register access, RAL/RAH MAC autoloaded from the EEPROM at reset,
     ring-based RX/TX with `RDT` treated as exclusive.
   - Boot order is rtl8139 (initcall 16) → e1000e (17) → virtio-net (18);
     priority, not boot order, decides which one the protocol layer actually
     dispatches through.

3. **virtio-net (`src/drivers/virtio_net.c`)**, v38.174 — see the section
   below.

4. **Network Address Defaults (QEMU Slirp)**:
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

---

## 🔌 virtio-net (v38.174)

The third backend and the first paravirtual one. It shares rings with the
hypervisor instead of emulating a card, which is what a KVM guest would
normally boot on. `net.h`'s vtable was introduced for exactly this — the
protocol layer above it is untouched.

**Attach:**

```sh
qemu-system-i386 ... -netdev user,id=n0 \
    -device virtio-net-pci,disable-modern=on,netdev=n0
```

`disable-modern=on` is required: the kernel targets the **legacy**
(transitional) virtio-pci interface — one I/O BAR, PIO registers, no MSI-X —
for the same reason virtio-blk does (`src/include/virtio.h`). Modern-only
devices (`0x1040+`) are skipped by the probe.

**Transport**, mirroring `vblk_hw_init()` in `src/drivers/virtio_blk.c`:
BAR0 must be an I/O BAR; enable I/O + bus master; status handshake
(`ACK` → `DRIVER` → `FEATURES_OK` → `DRIVER_OK`); negotiate a minimal feature
set (**MAC only**); lay out each ring from the device's own `QueueNum`
(legacy has no size register — the device derives every ring address from the
size it reports); publish it with `QUEUE_PFN`.

**Two virtqueues** (§5.1.2): `0` = receiveq, `1` = transmitq. Each needs its
own page-aligned ring block, because `QUEUE_PFN` is a frame number.

**The 10-byte header is the bring-up landmine.** Every buffer in both
directions begins with `virtio_net_hdr_t`:

- TX descriptor length is `VNET_HDR_SIZE + frame_len`, not `frame_len`.
- The RX used-ring length **includes** the header, so the Ethernet frame
  starts at `buf + VNET_HDR_SIZE`.

It is 10 bytes and not 12 because the driver negotiates neither
`VIRTIO_NET_F_MRG_RXBUF` (which would append a 16-bit `num_buffers`) nor
`VIRTIO_F_VERSION_1`. Negotiating either would silently shift every frame by
two bytes.

**Poll-only.** `avail.flags = VIRTQ_AVAIL_F_NO_INTERRUPT` keeps the device
from raising INTx, and `net_poll()` drains up to four frames per call from the
main loop through `net_backend->poll_rx()` — the same path rtl8139 and e1000e
use as a fallback. **No IRQ is wired**; RX latency therefore follows how often
`net_poll()` runs. If that ever proves too slow, the fix is to route the
device's IRQ into `net_irq_handler`, not to change this design.

**MAC** comes from the first 6 bytes of the legacy config space when
`VIRTIO_NET_F_MAC` is offered (it always is under QEMU). If it is not, the
driver synthesizes a deterministic locally-administered address rather than
shipping `00:00:00:00:00:00`, which would make DHCP send a chaddr nobody owns.

**Gates:** `scripts/dhcp_test.py --nic virtio-net` (broadcast TX, RX, ARP,
DHCP) and `scripts/socktest.py --nic virtio-net` (TCP client, hostfwd
PING/PONG, server accept). CI job `browser-net-virtio` runs both.
