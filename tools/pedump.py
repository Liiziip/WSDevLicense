#!/usr/bin/env python3
"""Minimal PE inspector: headers, sections, imports, exports, resources."""
import struct, sys

MACHINES = {0x014c: "I386", 0x01c0: "ARM", 0x01c4: "ARMNT (ARMv7 Thumb-2)",
            0xaa64: "ARM64", 0x8664: "AMD64", 0x0200: "IA64", 0x0166: "MIPS"}
SUBSYS = {1: "NATIVE", 2: "WINDOWS_GUI", 3: "WINDOWS_CUI", 9: "WINCE_GUI", 10: "EFI_APP"}


def u8(b, o): return b[o]
def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]


class PE:
    def __init__(self, data):
        self.d = data
        assert data[:2] == b"MZ", "no MZ"
        e = u32(data, 0x3C)
        assert data[e:e + 4] == b"PE\0\0", "no PE sig"
        self.e = e
        self.machine = u16(data, e + 4)
        self.nsec = u16(data, e + 6)
        self.tds = u32(data, e + 8)
        self.optsize = u16(data, e + 20)
        self.chars = u16(data, e + 22)
        o = e + 24
        self.o = o
        self.magic = u16(data, o)
        self.pe32plus = self.magic == 0x20b
        self.entry = u32(data, o + 16)
        self.base = u32(data, o + 28) if not self.pe32plus else struct.unpack_from("<Q", data, o + 24)[0]
        self.subsystem = u16(data, o + 68)
        self.dllchar = u16(data, o + 70)
        self.sizeofimage = u32(data, o + 56)
        self.sizeofheaders = u32(data, o + 60)
        self.nrva = u32(data, o + (108 if self.pe32plus else 92))
        self.dirs = []
        dd = o + (112 if self.pe32plus else 96)
        for i in range(16):
            self.dirs.append((u32(data, dd + i * 8), u32(data, dd + i * 8 + 4)))
        self.sections = []
        so = o + self.optsize
        for i in range(self.nsec):
            s = so + i * 40
            name = data[s:s + 8].rstrip(b"\0").decode("latin1")
            self.sections.append(dict(name=name, vsize=u32(data, s + 8), va=u32(data, s + 12),
                                      rsize=u32(data, s + 16), rptr=u32(data, s + 20),
                                      chars=u32(data, s + 36)))

    def off(self, rva):
        for s in self.sections:
            if s["va"] <= rva < s["va"] + max(s["vsize"], s["rsize"]):
                return rva - s["va"] + s["rptr"]
        return None

    def rva_bytes(self, rva, n):
        o = self.off(rva)
        return self.d[o:o + n] if o is not None else b""

    def cstr(self, rva):
        o = self.off(rva)
        if o is None:
            return "?"
        end = self.d.find(b"\0", o)
        return self.d[o:end].decode("latin1", "replace")

    def imports(self):
        out = []
        rva, size = self.dirs[1]
        if not rva:
            return out
        o = self.off(rva)
        if o is None:
            return out
        i = 0
        while True:
            oft = u32(self.d, o + i * 20)
            name = u32(self.d, o + i * 20 + 12)
            first = u32(self.d, o + i * 20 + 16)
            if not (oft or name or first):
                break
            mod = self.cstr(name)
            names = []
            t = oft or first
            if t:
                thunk = self.off(t)
                thunksize = 8 if self.pe32plus else 4
                j = 0
                while thunk is not None:
                    v = (struct.unpack_from("<Q", self.d, thunk + j * thunksize)[0]
                         if self.pe32plus else u32(self.d, thunk + j * thunksize))
                    if v == 0:
                        break
                    if v & (1 << (63 if self.pe32plus else 31)):
                        names.append("#%d" % (v & 0xFFFF))
                    else:
                        names.append(self.cstr((v & 0x7FFFFFFF) + 2))
                    j += 1
                    if j > 4096:
                        break
            out.append((mod, names))
            i += 1
            if i > 512:
                break
        return out

    def exports(self):
        rva, size = self.dirs[0]
        out = []
        if not rva:
            return out
        o = self.off(rva)
        nnames = u32(self.d, o + 24)
        namesrva = u32(self.d, o + 32)
        no = self.off(namesrva)
        for i in range(nnames):
            nr = u32(self.d, no + i * 4)
            out.append(self.cstr(nr))
        return out

    def delay_imports(self):
        rva, size = self.dirs[13]
        out = []
        if not rva:
            return out
        o = self.off(rva)
        if o is None:
            return out
        i = 0
        while True:
            attrs = u32(self.d, o + i * 32)
            name = u32(self.d, o + i * 32 + 4)
            if not (attrs or name):
                break
            out.append((self.cstr(name), attrs))
            i += 1
            if i > 128:
                break
        return out


def main(path):
    d = open(path, "rb").read()
    p = PE(d)
    import datetime
    print("== %s  (%d bytes) ==" % (path, len(d)))
    print("Machine      : 0x%04X %s" % (p.machine, MACHINES.get(p.machine, "?")))
    print("Timestamp    : %s UTC" % datetime.datetime.utcfromtimestamp(p.tds).isoformat())
    print("Subsystem    : %d %s" % (p.subsystem, SUBSYS.get(p.subsystem, "?")))
    print("ImageBase    : 0x%X   SizeOfImage 0x%X" % (p.base, p.sizeofimage))
    print("Entry RVA    : 0x%X   (VA 0x%X)" % (p.entry, p.base + p.entry))
    print("DllChars     : 0x%04X" % p.dllchar)
    print("\nSections:")
    print("  %-10s %10s %10s %10s %10s  %s" % ("name", "VA", "VSize", "RawPtr", "RawSize", "Chars"))
    for s in p.sections:
        print("  %-10s %10X %10X %10X %10X  0x%08X" %
              (s["name"], s["va"], s["vsize"], s["rptr"], s["rsize"], s["chars"]))
    print("\nDataDirectories:")
    ddnames = ["export", "import", "resource", "exception", "security", "reloc", "debug",
               "arch", "globalptr", "tls", "loadcfg", "boundimp", "iat", "delayimp", "com",
               "reserved"]
    for i, (rva, sz) in enumerate(p.dirs):
        if rva or sz:
            print("  %-10s RVA=0x%-8X size=0x%X" % (ddnames[i], rva, sz))
    print("\nExports:")
    ex = p.exports()
    print("  %s" % (", ".join(ex) if ex else "(none)"))
    print("\nImports:")
    for mod, names in p.imports():
        print("  %s (%d)" % (mod, len(names)))
        for n in names:
            print("      %s" % n)
    di = p.delay_imports()
    if di:
        print("\nDelay imports:")
        for mod, attrs in di:
            print("  %s (attrs=0x%X)" % (mod, attrs))


if __name__ == "__main__":
    main(sys.argv[1])
