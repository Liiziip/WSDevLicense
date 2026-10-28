#!/usr/bin/env python3
"""MIDL NDR32 format-string oracle for the arm.exe reconstruction.

The format strings inside arm.exe are MIDL output; this tool regenerates them
from candidate IDL/ACF text with the locally installed midl and compares the
bytes, so a parameter table can be *proved* instead of guessed.

Commands
--------
  arm                        dump the two format-string blobs from arm.exe
  try NAME ACF BODY [ENTRY]  build a stub for BODY and diff entry ENTRY
                             (default 0x5e = proc2) against arm.exe
  dump FILE_c.c              dump offsets/proc/type of an existing stub

ACF is one of: none, explicit, implicit.
BODY is the interface body, e.g.
  "HRESULT Invoke([in] int s, [in] int o, [in] int cb, [in,size_is(cb)] byte *p);"
"""
import os
import re
import struct
import subprocess
import sys

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MIDL = os.environ.get("MIDL") or r"D:\Windows Kits\10\bin\10.0.17763.0\x86\midl.exe"
VCVARS = os.environ.get("VCVARS") or \
    r"D:\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars32.bat"
ARM = os.path.join(BASE, "arm.exe")          # your own copy, not in the repo
WORK = os.path.join(BASE, "tests", "midl", "gen")

PROC_VA, TYPE_VA = 0x401890, 0x40199A


# ------------------------------------------------------------------ arm.exe

def _sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = pe + 24
    base = struct.unpack_from("<I", data, opt + 28)[0]
    so = opt + struct.unpack_from("<H", data, pe + 20)[0]
    out = []
    for i in range(nsec):
        o = so + i * 40
        vsz, va, rsz, ra = struct.unpack_from("<IIII", data, o + 8)
        out.append((base + va, ra, rsz))
    return out


def arm_bytes(va, n):
    data = open(ARM, "rb").read()
    for base, ra, rsz in _sections(data):
        if base <= va < base + rsz:
            return data[ra + va - base: ra + va - base + n]
    raise ValueError("unmapped %#x" % va)


def arm_proc():
    return arm_bytes(PROC_VA + 2, 0xEE)   # Format[0]; the struct pad is separate


def arm_type():
    return arm_bytes(TYPE_VA, 0x9E)


# ------------------------------------------------------------ midl parsing

TOK = re.compile(r"NdrFcLong\(\s*(0x[0-9a-fA-F]+|\d+)\s*\)"
                 r"|NdrFcShort\(\s*(0x[0-9a-fA-F]+|\d+)\s*\)"
                 r"|(0x[0-9a-fA-F]+|\d+)")


def _block(text, name):
    m = re.search(r"__MIDL_" + name + r"\s*=\s*\{", text)
    if not m:
        raise ValueError("no __MIDL_" + name)
    body = text[m.end():]
    end = body.find("};")
    if end > 0:
        body = body[:end]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    out = bytearray()
    for mm in TOK.finditer(body):
        if mm.group(1) is not None:
            out += int(mm.group(1), 0).to_bytes(4, "little")
        elif mm.group(2) is not None:
            out += (int(mm.group(2), 0) & 0xFFFF).to_bytes(2, "little")
        else:
            out.append(int(mm.group(3), 0) & 0xFF)
    return bytes(out)


def parse_stub(path):
    text = open(path, encoding="latin-1").read()
    # the generated struct is { short Pad; unsigned char Format[] }; the token
    # parser yields one byte for the Pad token, so drop one byte, not two
    proc = _block(text, "ProcFormatString")[1:]
    typ = _block(text, "TypeFormatString")[1:]
    m = re.search(r"FormatStringOffsetTable\[\]\s*=\s*\{(.*?)\};", text, re.S)
    offs = [int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+|\d+", m.group(1))] if m else []
    return proc, typ, offs


# --------------------------------------------------------------- midl driver

IDL_HEAD = """import "unknwn.idl";
[
  uuid(C453C318-B4A8-4188-80C4-73E6C7417DBF),
  version(1.0),
  pointer_default(unique)
]
interface Wss
{
%s
}
"""

ACF = {
    "none": "interface Wss\n{\n}\n",
    "explicit": "[\n    explicit_handle\n]\ninterface Wss\n{\n}\n",
    "implicit": "[\n    implicit_handle(handle_t hWss)\n]\ninterface Wss\n{\n}\n",
}


def build(name, body, acf_kind="none", switches=""):
    os.makedirs(WORK, exist_ok=True)
    idl = os.path.join(WORK, name + ".idl")
    acf = os.path.join(WORK, name + ".acf")
    for p in (idl, acf):
        if os.path.exists(p):
            os.remove(p)
    for suff in ("_c.c", "_s.c", ".h"):
        p = os.path.join(WORK, name + suff)
        if os.path.exists(p):
            os.remove(p)
    open(idl, "w").write(IDL_HEAD % body)
    open(acf, "w").write(ACF[acf_kind])
    cmd = os.path.join(WORK, name + ".cmd")
    with open(cmd, "w") as f:
        f.write('@echo off\ncall "%s" >nul\n"%s" /nologo /env win32 /ms_ext /c_ext %s '
                '/acf "%s" /header "%s.h" /cstub "%s_c.c" /sstub "%s_s.c" "%s"\n'
                % (VCVARS, MIDL, switches, acf, os.path.join(WORK, name),
                   os.path.join(WORK, name), os.path.join(WORK, name), idl))
    p = subprocess.run(["cmd.exe", "/c", cmd], capture_output=True, text=True)
    cfile = os.path.join(WORK, name + "_c.c")
    if not os.path.exists(cfile):
        return None, (p.stdout + p.stderr)
    return parse_stub(cfile), (p.stdout + p.stderr)


# ----------------------------------------------------------------- printing

def parse_entry(b):
    """Split a NDR32 procedure format entry into its fields.

    Layout (confirmed against midl 8.01 output, see docs/ndr-proc2.md):

        +0   u8      (unused, 0)
        +1   u8      old flags (0x48)
        +2   u32     reserved (0)
        +6   u16     procedure index
        +8   u16     stack size in bytes
        +10  ...     handle descriptor: FC_BIND_xxx, flags, u16 stack offset
        ..   u16*    one or two buffer-size hints
        ..   u8      Oi2 flags
        ..   u8      parameter count (including the return value)
        ..   u8      size of the extension block, in bytes, counting itself
        ..   ...     extension block (NDR_PROC_HEADER_EXTS then per-version extras)
        ..   6*n     parameter descriptors: u16 flags, u16 stack offset, u16 type
    """
    n = len(b)
    for i in range(10, min(32, n - 3)):
        oi2, count, ext = b[i], b[i + 1], b[i + 2]
        if not oi2 & 0x40:            # HasExtensions must be set
            continue
        if not 1 <= count <= 16:
            continue
        if ext < 8:
            continue
        ps = i + 2 + ext
        if ps + 6 * count <= n and n - (ps + 6 * count) < 8:
            return {
                "header": b[:10],
                "handle": b[10:i],
                "oi2": oi2,
                "count": count,
                "ext": b[i + 2:i + 2 + ext],
                "params": [b[ps + 6 * k:ps + 6 * k + 6] for k in range(count)],
                "pad": b[ps + 6 * count:],
            }
    raise ValueError("not a procedure format entry: %s" % b.hex())


def is_basetype(flags):
    return bool(flags & 0x0040)


FC_NAME = {
    0x01: "FC_BYTE", 0x02: "FC_CHAR", 0x03: "FC_SMALL", 0x04: "FC_USMALL",
    0x05: "FC_WCHAR", 0x06: "FC_SHORT", 0x07: "FC_USHORT", 0x08: "FC_LONG",
    0x09: "FC_ULONG", 0x0a: "FC_FLOAT", 0x0b: "FC_HYPER", 0x0c: "FC_DOUBLE",
    0x0d: "FC_ENUM16", 0x0e: "FC_ENUM32", 0x0f: "FC_IGNORE",
    0x10: "FC_ERROR_STATUS_T", 0x11: "FC_RP", 0x12: "FC_UP", 0x13: "FC_OP",
    0x14: "FC_FP", 0x1b: "FC_CARRAY", 0x1c: "FC_CVARRAY", 0x30: "FC_BIND_CONTEXT",
    0x31: "FC_BIND_GENERIC", 0x32: "FC_BIND_PRIMITIVE", 0x33: "FC_AUTO_HANDLE",
    0x34: "FC_CALLBACK_HANDLE", 0x54: "FC_DEREFERENCE", 0x5b: "FC_END",
    0xb7: "FC_RANGE",
}

PARAM_FLAGS = [
    (0x0001, "MustSize"), (0x0002, "MustFree"), (0x0004, "IsPipe"),
    (0x0008, "IsIn"), (0x0010, "IsOut"), (0x0020, "IsReturn"),
    (0x0040, "IsBasetype"), (0x0080, "IsByValue"), (0x0100, "IsSimpleRef"),
    (0x0200, "IsDontCallFreeInst"), (0x0400, "SaveForAsyncFinish"),
]

HANDLE_FLAGS = [(0x80, "VIA_PTR"), (0x40, "IN"), (0x20, "OUT"), (0x10, "RETURN"),
                (0x08, "STRICT"), (0x04, "NOSErialize"),
                (0x02, "serialize"), (0x01, "CANNOT_BE_NULL")]

OI2_FLAGS = [(0x01, "ServerMustSize"), (0x02, "ClientMustSize"),
             (0x04, "HasReturn"), (0x08, "HasPipes"), (0x20, "HasAsyncUuid"),
             (0x40, "HasExtensions"), (0x80, "HasAsyncHandle")]

EXT_FLAGS2 = [(0x01, "HasNewCorrDesc"), (0x02, "ClientCorrCheck"),
              (0x04, "ServerCorrCheck"), (0x08, "HasNotify"),
              (0x10, "HasNotify2"), (0x20, "has complex return"),
              (0x40, "has range on conformance")]


def flagstr(val, table):
    out = [name for bit, name in table if val & bit]
    known = 0
    for bit, _ in table:
        known |= bit
    rest = val & ~known
    if rest:
        out.append("0x%02x" % rest)
    return "|".join(out) if out else "0"


def describe_entry(b, label=""):
    e = parse_entry(b)
    h = e["handle"]
    ht = h[0]
    lines = []
    lines.append("%-28s %d bytes" % (label or "entry", len(b)))
    lines.append("  handle      %s flags=%s offset=0x%04x (%s)" % (
        FC_NAME.get(ht, "FC_0x%02x" % ht),
        flagstr(h[1] if len(h) > 1 else 0, HANDLE_FLAGS),
        (h[2] | (h[3] << 8)) if len(h) >= 4 else 0,
        " ".join("%02x" % x for x in h)))
    hints = b[10 + len(h):len(b) - (2 + len(e["ext"]) + 6 * e["count"])]
    lines.append("  hints       %s" % " ".join("%02x" % x for x in hints))
    lines.append("  Oi2         %s  count=%d" % (flagstr(e["oi2"], OI2_FLAGS),
                                                e["count"]))
    lines.append("  ext         size=%d flags2=%s" % (
        len(e["ext"]), flagstr(e["ext"][1], EXT_FLAGS2) if len(e["ext"]) > 1 else "-"))
    for k, p in enumerate(e["params"]):
        fl = p[0] | (p[1] << 8)
        of = p[2] | (p[3] << 8)
        ty = p[4] | (p[5] << 8)
        tn = FC_NAME.get(ty, "type+0x%02x" % ty) if is_basetype(fl) else "type+0x%02x" % ty
        lines.append("  param %d     %-52s off=%2d  %s" % (
            k, flagstr(fl, PARAM_FLAGS), of, tn))
    return "\n".join(lines)


FINAL_BODY = """typedef [context_handle] void *WSSCTX;
typedef enum _OP { OP_INSTALL = 0, OP_REMOVE = 0xc } OP;

HRESULT Open([out] WSSCTX *pctx);
HRESULT Close([in,out] WSSCTX *pctx);
HRESULT Invoke([in] WSSCTX ctx, [in] OP op,
               [in, range(0,0x100000)] unsigned long cb,
               [in,size_is(cb)] char p[],
               [out] long *pcb,
               [out,size_is(,*pcb)] char **pp);
"""


def arm_entry(off, end_off=None):
    """arm.exe entry at `off` (an offset from the 0x401890 table base).

    Entry boundaries are taken from the table: published offsets are 0x02
    (proc0), 0x30 (proc1), 0x5e (proc2) and 0xb0 (proc3, an entry the tool
    never calls); 0x10a is where the type format string starts.
    """
    arm = arm_proc()          # starts at Format[0], i.e. table base + 2
    start = off - 2
    end = (end_off - 2) if end_off else len(arm)
    return arm[start:end]


def verify():
    """Build FINAL_BODY with midl and compare every field with arm.exe."""
    res, log = build("final", FINAL_BODY, "explicit")
    if res is None:
        print(log)
        return 1
    proc, typ, offs = res
    out = ["midl 8.01.0622  (switches: /env win32 /ms_ext /c_ext /acf explicit_handle)",
           "entry offsets: %s" % [hex(x) for x in offs],
           "IDL/ACF used: see tools/ndr_proc.py FINAL_BODY", ""]
    for idx, (va, va_end) in enumerate(((0x02, 0x30), (0x30, 0x5E), (0x5E, 0xB0))):
        a = arm_entry(va, va_end)
        end = offs[idx + 1] if idx + 1 < len(offs) else len(proc)
        m = proc[offs[idx]:end]
        out += ["=" * 78,
                "arm.exe VA %#x (%d bytes)    midl reference (%d bytes)" % (va, len(a), len(m)),
                "=" * 78,
                "ARM:", describe_entry(a, "  arm.exe"),
                "MIDL:", describe_entry(m, "  midl")]
        ea, em = parse_entry(a), parse_entry(m)
        diffs = []
        for field in ("header", "handle", "oi2", "count", "ext", "params", "pad"):
            fa, fm = ea[field], em[field]
            if field == "params":
                for k, (pa, pm) in enumerate(zip(fa, fm)):
                    if pa != pm:
                        diffs.append("param %d: arm %s   midl %s"
                                     % (k, pa.hex(" "), pm.hex(" ")))
            elif isinstance(fa, bytes):
                if fa != fm:
                    diffs.append("%-8s arm %s   midl %s"
                                 % (field, fa.hex(" ") or "-", fm.hex(" ") or "-"))
            elif fa != fm:
                diffs.append("%-8s arm %#x   midl %#x" % (field, fa, fm))
        out.append("field differences:")
        out += ["  " + d for d in diffs] if diffs else ["  none"]
        out.append("")
    a, m = arm_type(), typ
    out += ["=" * 78,
            "type format string: arm %d bytes, midl %d bytes" % (len(a), len(m)),
            "=" * 78,
            "arm : " + a.hex(" "),
            "midl: " + m.hex(" ")]
    text = "\n".join(out)
    print(text)
    dest = os.path.join(BASE, "docs", "ndr-verify.txt")
    with open(dest, "w") as f:
        f.write(text + "\n")
    print("\nwrote %s" % dest)
    return 0


def hexs(b, base=0, per=16):
    return "\n".join("%04x  %s" % (base + i, " ".join("%02x" % x for x in b[i:i + per]))
                     for i in range(0, len(b), per))


def common_prefix(a, b):
    n = 0
    while n < len(a) and n < len(b) and a[n] == b[n]:
        n += 1
    return n


def show_cmp(label, mine, ref):
    n = common_prefix(mine, ref)
    print("  %-28s %3d/%3d bytes equal (differs at +%#x)"
          % (label, n, len(ref), n))
    if n < min(len(mine), len(ref)):
        lo = max(0, n - 8)
        hi = min(max(len(mine), len(ref)), n + 24)
        print("      ref  +%03x: %s" % (lo, " ".join("%02x" % x for x in ref[lo:hi])))
        print("      mine +%03x: %s" % (lo, " ".join("%02x" % x for x in mine[lo:hi])))
        # byte-wise diff in the whole overlap
        d = [(i, mine[i], ref[i]) for i in range(min(len(mine), len(ref)))
             if mine[i] != ref[i]]
        print("      %d differing bytes: %s" % (
            len(d), ", ".join("+%#04x %02x!=%02x" % x for x in d[:16])))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    cmd = argv[1]
    if cmd == "arm":
        print("proc format string (base 0x401890, Format[0] at +0x02):")
        print(hexs(arm_proc()))
        print("\ntype format string (0x40199a):")
        print(hexs(arm_type()))
        return 0
    if cmd == "dump":
        proc, typ, offs = parse_stub(argv[2])
        print("entry offsets:", [hex(x) for x in offs])
        print("proc:"); print(hexs(proc))
        print("type:"); print(hexs(typ))
        return 0
    if cmd == "verify":
        return verify()
    if cmd == "try":
        name, acf, body = argv[2], argv[3], argv[4]
        entry = int(argv[5], 0) if len(argv) > 5 else 0x5E
        res, log = build(name, body, acf)
        if res is None:
            print(log)
            return 1
        proc, typ, offs = res
        print("entry offsets:", [hex(x) for x in offs])
        print(hexs(proc))
        ref = arm_proc()
        # ref starts at Format[0] (VA 0x401892); entries are addressed the same way
        start = entry - (PROC_VA + 2)
        end = start + 1
        while end < len(ref) - 1:
            if ref[end] == 0 and ref[end + 1] == 0x48 and end > start + 8:
                break
            end += 1
        target = ref[start:end]
        print("\narm entry @%#x (%d bytes):" % (entry, len(target)))
        print(hexs(target))
        print("\ncompare with arm entry:")
        show_cmp("proc entry", proc, target)
        show_cmp("type format", typ, arm_type())
        return 0
    print("unknown command")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
