# Stage 1 — decode the NDR procedure format strings

Goal: recover the exact parameter list of `proc0` (open) and `proc2` (invoke) so the
call can be marshalled by hand instead of through `NdrClientCall2`.

## Raw bytes (from arm.exe, `.text` at VA 0x401890)

```
proc0  @ VA 0x401892 (table base 0x401890, entry offset +0x02)
00 48 00 00 00 00 00 00 0c 00 32 00 00 00 00 00
40 00 44 02 0e 41 00 00 00 00 00 00 02 00 02 80
81 00 10 01 04 00 06 00 70 00 08 00 08 00

proc1  @ VA 0x4018C0 (offset +0x30)
00 48 00 00 00 00 01 00 08 00 30 e8 00 00 00 00
38 00 40 00 44 02 0c 41 00 00 00 00 00 00 01 00
01 80 18 01 00 00 0e 00 70 00 04 00 08 00 00 48

proc2  @ VA 0x4018EE (offset +0x5e)
00 48 00 00 00 00 02 00 1c 00 30 48 00 00 00 00
32 00 24 00 47 07 12 47 01 00 01 00 00 00 06 00
06 80 81 82 83 fc fc 00 08 00 00 00 12 00 48 00
04 00 0d 00 88 00 08 00 16 00 0b 00 0c 00 20 00
50 21 10 00 08 00 13 20 14 00 3a 00 70 00 18 00
08 00 00 48 00 00 00 00 03 00 20 00 30 48 04 00
00 00 32 00 24 00 c7 07 12 47 01 00 01 00 00 00
07 00 07 80 81 82 83 fc fc fc 08 00 04 00 12 00
48 00 08 00 0d 00 88 00 0c 00 58 00 0b 00 10 00
62 00 50 21 14 00 08 00 13 20 18 00 78 00 70 00
1c 00 08 00 00 00 00 00
```

Type format string: `arm.exe` VA 0x40199A, 0x9E bytes (see `src/midl_blob.c`).
`procN` type offset field (bytes +16/+17 of each entry): proc0 = 0x0040, proc2 = 0x0024/0x0032.

## What is already established

* `FC_BIND_PRIMITIVE` (`0x32`) at entry offset +10 for proc0, so the binding is the first
  *argument* (`&binding`), and the remaining parameters follow.
* From the WinDbg capture of the working x64 build (`work/ndr_capture2.log`):
  `proc2(ctx_pointer, opcode, 1240, buffer, &resp, &resplen)` — length before buffer.
* x64 `proc0(binding, &ctx)` — the server writes a context handle there; the context is a
  16-byte GUID + 4-byte attributes on the wire.

## Next concrete step

**Done — see [ndr-proc2.md](ndr-proc2.md).** The entry at `0x4018EE` is decoded
(`FC_BIND_CONTEXT` session, 7 dwords, parameter table) and verified against a
midl-generated reference stub with `python tools\ndr_proc.py verify`. The open
question this document ends with is answered there: the "session" is a 20-byte
context handle, and the remaining byte differences are the midl-7 extension
block and the `[range]`-in-correlation-descriptor encoding.

Historical notes below (kept for the record).

Decode the parameter descriptors of `proc0`/`proc2` against the NDR format-char table
(FC_* constants) and the type-format string at 0x40199A. Tooling that already exists:

* `tools/ndr_proc.py` — builds a reference stub for a candidate IDL with midl and
  diffs its format bytes against a target (`verify` subcommand reproduces the
  final result; it superseded an earlier `midl_probe.py`).
* `tests/midl/explicit_c.c` — a midl-generated reference whose bytes match the head of
  proc0 exactly (`00 48 00 00 00 00 00 00 0c 00 32 00 ...`), i.e. proc0 really is
  `(handle, [out] something)`.

Once the descriptors are known, the encoder is a direct translation.
