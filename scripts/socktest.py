#!/usr/bin/env python3
"""
scripts/socktest.py — regression test for the POSIX socket API (v38.43).

Boots mectov.iso in QEMU with user networking (rtl8139 + hostfwd) and a
host-side echo server, then runs /apps/tcpserver.mct which exercises the
fd-integrated socket syscalls in both directions:

  1. guest CLIENT:  socket/connect to 10.0.2.2:9999, write, read the echo
                    back                                        ("client OK")
  2. guest SERVER:  socket/bind/listen on :8080; the host connects
                    through hostfwd, sends PING, expects PONG    ("server OK")

Covers SYS_SOCKET/BIND/LISTEN/ACCEPT/CONNECT, read/write on socket fds,
poll() POLLOUT/POLLIN readiness, and accept()'s listener-stays-listening
model end to end.

Usage:
    python3 scripts/socktest.py [--timeout 240]
"""
import argparse
import os
import socket
import subprocess
import sys
import threading
import time

import terminal_launch

SERIAL_LOG = "/tmp/mectov_sock_serial.log"
MON_SOCK = "/tmp/mectov_sock_monitor.sock"

ECHO_PORT = 9999        # host echo server the guest client connects to
HOSTFWD_PORT = 15580    # host-side port forwarded to guest :8080

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
RUN_KEYS = ["r", "u", "n", "spc", "slash", "a", "p", "p", "s",
            "slash", "t", "c", "p", "s", "e", "r", "v", "e", "r", "dot", "m", "c", "t", "ret"]


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


def start_echo_server(stop_evt):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", ECHO_PORT))
    srv.listen(4)
    srv.settimeout(0.5)

    def serve():
        while not stop_evt.is_set():
            try:
                conn, _ = srv.accept()
            except socket.timeout:
                continue
            with conn:
                conn.settimeout(5)
                try:
                    data = conn.recv(1024)
                    if data:
                        conn.sendall(data)
                except OSError:
                    pass

    t = threading.Thread(target=serve, daemon=True)
    t.start()
    return srv


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

    stop_evt = threading.Event()
    echo_srv = start_echo_server(stop_evt)
    print(f"[OK] host echo server on 127.0.0.1:{ECHO_PORT}")

    qemu_cmd = [
        "qemu-system-i386",
        "-cpu", "qemu32,+nx",
        "-vga", "std",
        "-cdrom", args.iso,
        "-m", "128",
        "-smp", "4",
        "-display", "none",
        "-serial", f"file:{SERIAL_LOG}",
        # user networking: the guest reaches the host at 10.0.2.2; the host
        # reaches guest :8080 through the forwarded local port.
        "-netdev", f"user,id=n0,hostfwd=tcp:127.0.0.1:{HOSTFWD_PORT}-:8080",
        "-device", "rtl8139,netdev=n0",
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

        for k in LOGIN_KEYS:
            mon_cmd("sendkey " + k)
            time.sleep(0.15)

        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            return 1
        print("[OK] logged in, desktop running")

        time.sleep(1.5)
        if not terminal_launch.launch_terminal(
                mon_cmd, SERIAL_LOG, "/tmp/mectov_sock_cursor.ppm"):
            print("[FAIL] terminal did not launch (icon double-click missed?)")
            return 1
        print("[OK] terminal launched")
        if not wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30):
            print("[FAIL] terminal never became ready")
            return 1
        time.sleep(1.0)

        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)

        ok_run = False
        for _ in range(3):
            for _ in range(32):
                mon_cmd("sendkey backspace")
            for k in RUN_KEYS:
                mon_cmd("sendkey " + k)
                time.sleep(0.12)
            if wait_for_in_file(SERIAL_LOG, "[SOCK] start", 25):
                ok_run = True
                break
            time.sleep(1.0)
        if not ok_run:
            print("[FAIL] socket demo never started")
            return 1
        print("[OK] socket demo running")

        # Phase 1: guest client talks to our echo server.
        if not wait_for_in_file(SERIAL_LOG, "[SOCK] client OK", 90):
            print("[FAIL] client phase failed (no 'client OK')")
            return 1
        print("[OK] client socket/connect/read/write verified")

        # Phase 2: connect to the guest's listening socket through hostfwd.
        if not wait_for_in_file(SERIAL_LOG, "[SOCK] server listening on 8080", 30):
            print("[FAIL] server never started listening")
            return 1

        pong = b""
        try:
            c = socket.create_connection(("127.0.0.1", HOSTFWD_PORT), timeout=10)
            c.settimeout(15)
            c.sendall(b"PING\n")
            pong = c.recv(16)
            c.close()
        except OSError as e:
            print(f"[FAIL] host could not reach the guest server: {e}")
            return 1
        if pong != b"PONG\n":
            print(f"[FAIL] expected PONG, got {pong!r}")
            return 1
        print("[OK] host<->guest PING/PONG echo verified")

        if not wait_for_in_file(SERIAL_LOG, "[SOCK] server OK", 30):
            print("[FAIL] guest did not confirm the server echo")
            return 1
        print("[OK] server accept/read/write verified")

        time.sleep(5)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive after the socket test")
        return 0
    finally:
        stop_evt.set()
        try:
            echo_srv.close()
        except OSError:
            pass
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
