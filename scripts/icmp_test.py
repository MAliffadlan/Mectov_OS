#!/usr/bin/env python3
"""
scripts/icmp_test.py — ICMP echo handling over an injectable L2 link (audit C6).

Boots mectov.iso with a UDP-tunnel socket netdev instead of QEMU slirp, so the
test speaks raw Ethernet to the guest's rtl8139 and can inject exactly the
frames the RFCs describe (slirp can only produce well-behaved ones):

  1. echo request from a PEER whose MAC is NOT the gateway's → the reply must
     be addressed to that PEER (the old code always used gateway_mac, which is
     all zeroes on this link — the reply went nowhere);
  2. echo request with a corrupt ICMP checksum → dropped (RFC 792);
  3. a maximum-size echo (1472 data bytes, IP total 1500, frame 1514) → must
     be answered; the old rtl8139_send_packet() dropped any frame > 1500;
  4. an oversized echo (IP total 1504) → dropped without corrupting the
     stack; a following well-formed ping must still get its reply.

The guest needs no login: the kernel's network stack comes up on its own
(DHCP retries, then the static fallback marks the interface BOUND, which is
what lets unicast frames through net_handle_ip()).

Usage:
    python3 scripts/icmp_test.py [--timeout 240]
"""
import argparse
import os
import select
import socket
import struct
import subprocess
import sys
import time

SERIAL_LOG = "/tmp/mectov_icmp_serial.log"
MON_SOCK = "/tmp/mectov_icmp_monitor.sock"

# UDP tunnel endpoint on the test side; QEMU binds the other port and sends
# every guest frame to us as one datagram (and vice versa).
UDP_TEST_PORT = 22443
UDP_QEMU_PORT = 22444

GUEST_IP = bytes((10, 0, 2, 15))       # net.c static fallback default
PEER_IP = bytes((192, 168, 7, 7))      # a host that is not the gateway
PEER_MAC = bytes((0x02, 0x11, 0x22, 0x33, 0x44, 0x55))
ICMP_ID = 0x4D43                        # 'MC'

ETH_TYPE_IP = 0x0800
IP_PROTO_ICMP = 1
ICMP_ECHO_REQUEST = 8
ICMP_ECHO_REPLY = 0


def wait_for_in_file(path, needle, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(path, "r", errors="replace") as f:
                if needle in f.read():
                    return True
        except (FileNotFoundError, OSError):
            pass
        time.sleep(0.5)
    return False


def csum(data):
    """One's-complement Internet checksum (big-endian words)."""
    if len(data) % 2:
        data += b"\x00"
    s = 0
    for i in range(0, len(data), 2):
        s += (data[i] << 8) | data[i + 1]
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def build_echo(guest_mac, seq, data, icmp_len=None, bad_checksum=False):
    """A full Ethernet frame: PEER -> GUEST, ICMP echo request."""
    if icmp_len is not None:
        cur = 8 + len(data)
        if cur < icmp_len:
            data = data + b"\x5a" * (icmp_len - cur)
        elif cur > icmp_len:
            data = data[:icmp_len - 8]
    icmp = struct.pack("!BBHHH", ICMP_ECHO_REQUEST, 0, 0, ICMP_ID, seq) + data
    c = csum(icmp)
    if bad_checksum:
        c ^= 0x5555
    icmp = icmp[:2] + struct.pack("!H", c) + icmp[4:]
    # src=PEER, dst=GUEST; checksum with the field zeroed, then filled in.
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(icmp), 0x1234, 0,
                     64, IP_PROTO_ICMP, 0, PEER_IP, GUEST_IP)
    ip = ip[:10] + struct.pack("!H", csum(ip)) + ip[12:]
    return guest_mac + PEER_MAC + struct.pack("!H", ETH_TYPE_IP) + ip + icmp


def parse_ipv4(frame):
    """Return (src_mac, dst_mac, iphdr, ihl, proto, payload) or None."""
    if len(frame) < 14 + 20:
        return None
    dst_mac, src_mac, etype = frame[:6], frame[6:12], struct.unpack("!H", frame[12:14])[0]
    if etype != ETH_TYPE_IP:
        return None
    ip = frame[14:]
    ihl = (ip[0] & 0x0F) * 4
    if (ip[0] >> 4) != 4 or ihl < 20 or len(ip) < ihl:
        return None
    total = struct.unpack("!H", ip[2:4])[0]
    if total > len(ip):
        total = len(ip)
    return src_mac, dst_mac, ip[:ihl], ihl, ip[9], ip[ihl:total]


def find_guest_mac(sock, deadline):
    """Learn the guest's MAC from any frame it transmits (DHCP DISCOVER...)."""
    while time.time() < deadline:
        r, _, _ = select.select([sock], [], [], 0.5)
        if not r:
            continue
        frame = sock.recv(4096)
        if len(frame) >= 14:
            return frame[6:12]
    return None


def recv_until(sock, deadline, match_id, match_seq):
    """Collect frames until one is an echo reply for (id, seq); return it or None."""
    while time.time() < deadline:
        r, _, _ = select.select([sock], [], [], 0.5)
        if not r:
            continue
        frame = sock.recv(4096)
        p = parse_ipv4(frame)
        if not p:
            continue
        src_mac, dst_mac, iphdr, ihl, proto, payload = p
        if proto != IP_PROTO_ICMP or len(payload) < 8:
            continue
        ctype, _code, _c, icmp_id, seq = struct.unpack("!BBHHH", payload[:8])
        if ctype == ICMP_ECHO_REPLY and icmp_id == match_id and seq == match_seq:
            return frame, p
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    args = ap.parse_args()

    for p in (SERIAL_LOG, MON_SOCK):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", UDP_TEST_PORT))
    sock.setblocking(False)

    # NOTE: no slirp — a raw UDP tunnel carries each Ethernet frame as one
    # datagram in both directions, so the test can inject arbitrary frames.
    qemu_cmd = [
        "qemu-system-i386",
        "-cpu", "qemu32,+nx",
        "-vga", "std",
        "-cdrom", args.iso,
        "-m", "128",
        "-smp", "4",
        "-display", "none",
        "-serial", f"file:{SERIAL_LOG}",
        "-netdev", f"socket,id=n0,udp=127.0.0.1:{UDP_TEST_PORT},localaddr=127.0.0.1:{UDP_QEMU_PORT}",
        "-device", "rtl8139,netdev=n0",
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ]
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    send_to = ("127.0.0.1", UDP_QEMU_PORT)
    failures = 0

    def send(frame):
        sock.sendto(frame, send_to)

    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] kernel never booted")
            return 1
        print("[OK] booted to login screen (no login needed for this test)")

        if not wait_for_in_file(SERIAL_LOG, "[DEV] net rtl8139 present=1", 10):
            print("[FAIL] the RTL8139 did not register as a NET device")
            return 1
        print("[OK] NIC registered as a NET device")

        # The static fallback marks the interface BOUND (dhcp_state), which is
        # what lets unicast frames past net_handle_ip's "for us?" gate.
        if not wait_for_in_file(SERIAL_LOG, "static config", 60):
            print("[FAIL] DHCP client never fell back to the static config")
            return 1
        print("[OK] network up on static config (BOUND, 10.0.2.15)")

        guest_mac = find_guest_mac(sock, time.time() + 10)
        if guest_mac is None:
            print("[FAIL] never saw a frame from the guest — UDP tunnel broken?")
            return 1
        print("[OK] guest MAC " + ":".join("%02x" % b for b in guest_mac))

        # Drain whatever the boot left queued (DISCOVER/ARP broadcasts).
        while True:
            r, _, _ = select.select([sock], [], [], 0.2)
            if not r:
                break
            sock.recv(4096)

        # ---- 1. Plain echo: answered, and addressed to the peer -------------
        send(build_echo(guest_mac, 1, b"MECTOV-ICMP-TEST"))
        got = recv_until(sock, time.time() + 5, ICMP_ID, 1)
        if not got:
            print("[FAIL] no echo reply to a well-formed request")
            failures += 1
        else:
            frame, (_, dst_mac, iphdr, ihl, proto, payload) = got
            ok = True
            if dst_mac != PEER_MAC:
                print("[FAIL] reply dst MAC is %s, expected the sender's %s" %
                      (":".join("%02x" % b for b in dst_mac), ":".join("%02x" % b for b in PEER_MAC)))
                ok = False
            if payload[8:] != b"MECTOV-ICMP-TEST":
                print("[FAIL] reply payload was not echoed back")
                ok = False
            if csum(iphdr) != 0 or csum(payload) != 0:
                print("[FAIL] reply carries an invalid IP or ICMP checksum")
                ok = False
            if ok:
                print("[OK] echo reply: addressed to the peer, payload echoed, checksums valid")
            else:
                failures += 1

        # ---- 2. Corrupt ICMP checksum: silently dropped ---------------------
        send(build_echo(guest_mac, 2, b"CORRUPT", bad_checksum=True))
        got = recv_until(sock, time.time() + 2, ICMP_ID, 2)
        if got:
            print("[FAIL] replied to an echo whose ICMP checksum does not verify")
            failures += 1
        else:
            print("[OK] corrupt-checksum echo dropped (RFC 792)")

        # ---- 3. Maximum-size echo: IP total 1500 / frame 1514 ---------------
        send(build_echo(guest_mac, 3, b"\x00" * 1472, icmp_len=1480))
        got = recv_until(sock, time.time() + 5, ICMP_ID, 3)
        if not got:
            print("[FAIL] no reply to a maximum-size (1472-byte payload) echo")
            failures += 1
        else:
            _, (_, dst_mac, iphdr, ihl, proto, payload) = got
            if len(payload) != 1480 or csum(payload) != 0:
                print("[FAIL] max-size reply truncated or corrupt (%d bytes)" % len(payload))
                failures += 1
            else:
                print("[OK] maximum-size echo (1472 data bytes) answered intact")

        # ---- 4. Oversized echo: dropped, stack unharmed ---------------------
        send(build_echo(guest_mac, 4, b"\x00" * 1476, icmp_len=1484))
        got = recv_until(sock, time.time() + 2, ICMP_ID, 4)
        if got:
            print("[FAIL] answered an oversized echo (IP total > MTU)")
            failures += 1
        else:
            print("[OK] oversized echo dropped")

        # ---- 5. Liveness: a fresh well-formed echo still gets its reply -----
        send(build_echo(guest_mac, 5, b"STILL-ALIVE"))
        got = recv_until(sock, time.time() + 5, ICMP_ID, 5)
        if not got:
            print("[FAIL] stack stopped answering after the oversized echo")
            failures += 1
        else:
            print("[OK] stack still answers a fresh echo (no corruption)")

        if failures:
            print("[FAIL] %d ICMP check(s) failed" % failures)
            return 1
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive; all ICMP checks passed")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass
        sock.close()


if __name__ == "__main__":
    sys.exit(main())
