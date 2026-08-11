#!/usr/bin/env python3
"""
scripts/dhcp_test.py — functional test for the in-kernel DHCP client (v38.11).

Boots mectov.iso in QEMU with the RTL8139 NIC + user-mode networking (QEMU
slirp has a built-in DHCP server), and verifies from the serial log that the
full RFC 2131 exchange completes and the runtime config is applied:

  1. client starts                         ("[NET] init: starting DHCP client")
  2. DISCOVER sent as UDP broadcast        ("[DHCP] DISCOVER")
  3. server OFFER received                 ("[DHCP] reply type=...02")
  4. REQUEST sent                          ("[DHCP] REQUEST")
  5. server ACK received                   ("[DHCP] reply type=...05")
  6. config applied at runtime             ("[DHCP] ACK — bound ... gw=... dns=...")
  7. gateway MAC resolved -> net_ready     ("[NET] Received ARP reply")
  8. the OS boots to the desktop loop and stays alive

Usage:
    python3 scripts/dhcp_test.py [--timeout 240]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

SERIAL_LOG = "/tmp/mectov_dhcp_serial.log"
MON_SOCK = "/tmp/mectov_dhcp_monitor.sock"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]


def wait_for_in_file(path, needle, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(path, "r", errors="replace") as f:
                if needle in f.read():
                    return True
        except (FileNotFoundError, OSError):
            pass
        time.sleep(1)
    return False


def mon_cmd(cmd):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(0.15)
        s.close()
    except OSError as e:
        print(f"[!] monitor cmd '{cmd}' failed: {e}")


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

    # NOTE: unlike the other tests this one NEEDS the NIC + user net: QEMU
    # slirp answers DHCP on 10.0.2.x. `-net none` tests verify the no-NIC
    # path (DHCP never runs there).
    qemu_cmd = [
        "qemu-system-i386",
        "-vga", "std",
        "-cdrom", args.iso,
        "-m", "128",
        "-smp", "4",
        "-display", "none",
        "-serial", f"file:{SERIAL_LOG}",
        "-net", "nic,model=rtl8139", "-net", "user",
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ]
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] kernel never reached login screen")
            return 1
        print("[OK] booted to login screen")

        if not wait_for_in_file(SERIAL_LOG, "[DHCP] DISCOVER", 30):
            print("[FAIL] DHCP DISCOVER never sent")
            return 1
        print("[OK] DISCOVER sent (UDP broadcast)")

        if not wait_for_in_file(SERIAL_LOG, "[DHCP] reply type=0x00000002", 30):
            print("[FAIL] no DHCP OFFER from server")
            return 1
        print("[OK] OFFER received")

        if not wait_for_in_file(SERIAL_LOG, "[DHCP] REQUEST", 20):
            print("[FAIL] DHCP REQUEST never sent")
            return 1
        print("[OK] REQUEST sent")

        if not wait_for_in_file(SERIAL_LOG, "[DHCP] reply type=0x00000005", 20):
            print("[FAIL] no DHCP ACK from server")
            return 1
        print("[OK] ACK received")

        if not wait_for_in_file(SERIAL_LOG, "[DHCP] ACK — bound 0x0A00020F", 20):
            print("[FAIL] config not applied (expected bound 10.0.2.15)")
            return 1
        print("[OK] runtime config applied (IP 10.0.2.15)")

        if not wait_for_in_file(SERIAL_LOG, "[NET] Received ARP reply", 30):
            print("[FAIL] gateway MAC never resolved after DHCP (net_ready off)")
            return 1
        print("[OK] gateway resolved, network ready")

        # Complete the login so the desktop loop is proven alive on DHCP config.
        for k in LOGIN_KEYS:
            mon_cmd("sendkey " + k)
            time.sleep(0.15)
        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete after DHCP")
            return 1
        print("[OK] logged in, desktop running")

        time.sleep(5)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive on DHCP-obtained config")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
