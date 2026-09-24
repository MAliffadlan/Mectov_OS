#!/usr/bin/env python3
"""
scripts/build_q3a_productid.py — write the `productid.txt` that id's own
filesystem looks for, derived from id's own check.

Why this file has to exist
--------------------------
`FS_SetRestrictions()` in code/qcommon/files.c decides whether the install
looks like the full game. If it decides it does not, it:

  * restarts the filesystem with the demo game directory (`demota`), and
  * sets `fs_restrict 1`, which makes FS_FOpenFileRead refuse every file that
    is not .cfg / .menu / .game / .dm_* / .dat **from a plain directory** —
    so a module on the ext2 volume (`baseq3/vm/qagame.qvm`) could never be
    opened, and a .pk3 we built ourselves could not pass the demo pak
    checksum either. There would be no way to load the bytecode at all.

The check is a straight read of `productid.txt` compared against a scrambled
constant baked into files.c, so the file that satisfies it is a pure function
of that constant. This script parses the constant out of the vendored
files.c and reproduces id's own loop:

    seed = 5000
    for each byte: expected[i] = scrambled[i] ^ (seed & 255)
                   seed = (69069 * seed + 1) mod 2**32

Nothing here grants access to any retail content — there is no retail content
anywhere in this project. It tells id's filesystem "this volume is a full
install", which is what makes it willing to read a game module off the volume
instead of dropping into demo mode.

Usage: scripts/build_q3a_productid.py [files.c] [out.txt]
"""
import re
import sys
import os

SRC_DEFAULT = "third_party/q3a/code/qcommon/files.c"
OUT_DEFAULT = "build/vm/productid.txt"


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else SRC_DEFAULT
    out = sys.argv[2] if len(sys.argv) > 2 else OUT_DEFAULT

    with open(src, "r") as f:
        text = f.read()

    m = re.search(r"fs_scrambledProductId\s*\[\s*(\d+)\s*\]\s*=\s*\{(.*?)\};",
                  text, re.S)
    if not m:
        print(f"[productid] cannot find fs_scrambledProductId in {src}",
              file=sys.stderr)
        return 1
    declared, body = int(m.group(1)), m.group(2)
    scrambled = [int(x) for x in re.findall(r"\d+", body)]
    if len(scrambled) != declared:
        print(f"[productid] expected {declared} bytes, parsed {len(scrambled)}",
              file=sys.stderr)
        return 1

    seed = 5000
    product = bytearray()
    for b in scrambled:
        product.append(b ^ (seed & 0xFF))
        seed = (69069 * seed + 1) & 0xFFFFFFFF

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "wb") as f:
        f.write(product)
    print(f"[productid] wrote {out} ({len(product)} bytes) from {src}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
