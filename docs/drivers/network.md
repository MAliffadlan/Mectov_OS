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

3. **Web Gateway Proxy Integration (`gateway.py`)**:
   - Outgoing HTTP connections on TCP port 80 are redirected by the network driver to `10.0.2.2:8888` (Host Web Gateway Proxy).
   - The python proxy fetches modern HTTPS web content, strips unnecessary bloat, and returns clean HTML/text readable by the Mectov OS Browser app.
