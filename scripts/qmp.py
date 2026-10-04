#!/usr/bin/env python3
"""QMP helper: send keys + run HMP commands with real replies.

Usage:
  python3 scripts/qmp.py <socket> keys r u n space a r g ret
  python3 scripts/qmp.py <socket> hmp "info qtree"
"""
import json
import socket
import sys


class QMP:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.f = self.s.makefile("rwb")
        self._read()  # greeting
        self.cmd("qmp_capabilities")

    def _read(self):
        line = self.f.readline()
        return json.loads(line) if line else {}

    def cmd(self, name, args=None):
        m = {"execute": name}
        if args:
            m["arguments"] = args
        self.f.write((json.dumps(m) + "\n").encode())
        self.f.flush()
        while True:
            r = self._read()
            if "return" in r or "error" in r:
                return r

    def sendkey(self, key):
        return self.cmd("send-key",
                        {"keys": [{"type": "qcode", "data": key}]})

    def hmp(self, cmdline):
        return self.cmd("human-monitor-command", {"command-line": cmdline})

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    q = QMP(sys.argv[1])
    mode = sys.argv[2]
    rc = 0
    if mode == "keys":
        import time
        for k in sys.argv[3:]:
            r = q.sendkey(k)
            if "error" in r:
                print(f"sendkey {k}: ERROR {r['error']}")
                rc = 1
            else:
                print(f"sendkey {k}: ok")
            time.sleep(0.15)
    elif mode == "hmp":
        r = q.hmp(" ".join(sys.argv[3:]))
        print(r.get("return", r))
    else:
        print("unknown mode", mode)
        rc = 1
    q.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
