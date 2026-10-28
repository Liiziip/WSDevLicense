#!/usr/bin/env python3
"""Hex dump a VA range of a PE, plus GUID-ish interpretation."""
import sys, struct
from pedump import PE

def main(path, va, n):
    d = open(path, "rb").read()
    p = PE(d)
    va = int(va, 0)
    n = int(n, 0)
    rva = va - p.base
    o = p.off(rva)
    if o is None:
        print("VA 0x%X not in any section" % va)
        return
    blob = d[o:o + n]
    for i in range(0, len(blob), 16):
        chunk = blob[i:i + 16]
        hexs = " ".join("%02x" % b for b in chunk)
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print("%08X  %-47s  %s" % (va + i, hexs, asc))

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3])
