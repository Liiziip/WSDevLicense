# proc2 — decoding the NDR procedure format entry at 0x4018EE

Target: `arm.exe`, procedure format string entry at **VA 0x4018EE** (table base
`0x401890` + `0x5E`; `Format[0]` is at `0x401892` because the MIDL blob is
`{ short Pad; unsigned char Format[]; }`).

Entry length is **82 bytes**: the next entry begins at `0x401940`
(`00 48 … 03 00 20 00`, procedure index 3), and `40 + 7·6 = 82`.

Verification: `python tools\ndr_proc.py verify` regenerates the reference stub
with midl 8.01.0622 and prints a field-by-field diff (also written to
`docs/ndr-verify.txt`).

## 1. Byte map

| off | bytes | field |
|---|---|---|
| +00 | `00` | (unused, 0 in every entry) |
| +01 | `48` | old flags (0x48 in every entry MIDL emits) |
| +02 | `00 00 00 00` | reserved, 0 |
| +06 | `02 00` | procedure index = 2 |
| +08 | `1c 00` | stack size = **28 bytes = 7 dwords** |
| +10 | `30` | **`FC_BIND_CONTEXT`** — the session is a *context handle*, not an int |
| +11 | `48` | handle flags: `HANDLE_PARAM_IS_IN (0x40)｜NDR_STRICT_CONTEXT_HANDLE (0x08)` |
| +12 | `00 00` | stack offset of the handle = 0 |
| +14 | `00 00` | (unused here; always 0 for this shape) |
| +16 | `32 00` | buffer-size hint 50 |
| +18 | `24 00` | buffer-size hint 36 |
| +20 | `47` | Oi2 flags: `ServerMustSize｜ClientMustSize｜HasReturn｜HasExtensions` |
| +21 | `07` | parameter count = **7** (6 parameters + return value) |
| +22 | `12` | extension block size = 18 bytes (counts itself) |
| +23 | `47` | ext flags2: `HasNewCorrDesc｜ClientCorrCheck｜ServerCorrCheck｜0x40` (0x40 = “has range on conformance”) |
| +24 | `01 00` | client correlation hint = 1 |
| +26 | `01 00` | server correlation hint = 1 |
| +28 | `00 00` | notify index = 0 |
| +30 | `06 00 06 80 81 82 83 fc fc 00` | midl-7 extension payload, 10 bytes (see §3) |
| +40 | 6 · 7 bytes | parameter descriptors from here to +81 |

The 7 descriptors start at **+40 = +22 + 18** — i.e. exactly after the
length-prefixed extension block. That is what proves the header size: with the
28-byte header MIDL 8 emits, 82 − 28 = 54 bytes could not hold 7 descriptors.

## 2. Parameter table

`(flags, stack offset, type)` triples are the raw 6-byte descriptors at
+40 … +81; `flags` bits are from `PARAM_ATTRIBUTES` in `ndrtypes.h`.

| # | stack off | raw flags | type field | meaning |
|---|---|---|---|---|
| 0 | 0 | `0x0008` IsIn | type+0x12 → `30 49 00 00` = `FC_BIND_CONTEXT`, flags `IN｜STRICT` | `[in] WSSCTX session` |
| 1 | 4 | `0x0048` IsIn｜IsBasetype | `0x0d` = `FC_ENUM16` | `[in] OP opcode` |
| 2 | 8 | `0x0088` IsIn｜IsByValue | type+0x16 → `b7 08` `FC_RANGE` over `FC_LONG`, min 0, max `0x00100000` | `[in, range(0,1048576)] unsigned long cb` |
| 3 | 12 | `0x000b` MustSize｜MustFree｜IsIn | type+0x20 → `1b 00 01 00` `FC_CARRAY`, elem size 1, corr desc `29 00 08 00` = parameter, `FC_ULONG`, stack offset 8 | `[in, size_is(cb)] char p[]` |
| 4 | 16 | `0x2150` IsOut｜IsBasetype｜IsSimpleRef｜ServerAllocSize=1 | `0x08` = `FC_LONG` | `[out] long *pcb` |
| 5 | 20 | `0x2013` MustSize｜MustFree｜IsOut｜ServerAllocSize=1 | type+0x3a → `FC_RP`→`FC_UP`→`FC_CARRAY`, corr desc `29 54 10 00` = parameter at stack offset 16, dereferenced | `[out, size_is(,*pcb)] char **pp` |
| 6 | 24 | `0x0070` IsOut｜IsReturn｜IsBasetype | `0x08` = `FC_LONG` | `HRESULT` return |

So on x86 the argument block for `NdrClientCall2` is 7 dwords wide:

```
[+0]  session  (context handle, 20 bytes on the wire)
[+4]  opcode   (enum, marshalled as 16-bit)
[+8]  cb       (range-checked ULONG)
[+12] p        (conformant array of char, size_is(cb))
[+16] pcb      ([out] long *)
[+20] pp       ([out] char **, size_is(*pcb))
[+24] return value slot
```

Independent cross-check — the disassembly of the ARM stub at `0x2ac4` builds
exactly this block:

```
r2 = session   r3 = opcode
[sp+0] = local8 (length)      [sp+4] = local0 (buffer)
[sp+8] = &local_c (length out) [sp+0xc] = &local_4 (pointer out)
```

and `FUN_00405030` (the serialiser) fills `local0`/`local8` with the request
pointer/length, with the 1 MiB cap applied to the length — matching param 2's
`FC_RANGE(0, 0x100000)`.

## 3. Reference stub and byte-for-byte verification

Reconstructed IDL/ACF (`tools/ndr_proc.py`, `FINAL_BODY` + `explicit_handle`):

```idl
typedef [context_handle] void *WSSCTX;
typedef enum _OP { OP_INSTALL = 0, OP_REMOVE = 0xc } OP;

HRESULT Open([out] WSSCTX *pctx);
HRESULT Close([in,out] WSSCTX *pctx);
HRESULT Invoke([in] WSSCTX ctx, [in] OP op,
               [in, range(0,0x100000)] unsigned long cb,
               [in,size_is(cb)] char p[],
               [out] long *pcb,
               [out,size_is(,*pcb)] char **pp);
```

Result for entry 0x4018EE (82 bytes vs midl's 73):

* header `00 48 00 00 00 00 02 00 1c 00` — **identical**;
* handle descriptor and both buffer hints `30 48 00 00 00 00 32 00 24 00` —
  identical except **one byte**: midl writes `40` (IN) where arm has `48`
  (IN｜STRICT);
* Oi2 flags / parameter count `47 07` — **identical**;
* **parameters 0,1,2,3,4,6 are byte-identical**; parameter 5 differs only in
  its type-string offset (`3a` vs `30`), a consequence of the extra range bytes
  in the type string (below);
* extension block: arm 18 bytes, midl 8 bytes — differs (see below).

Same picture for the other two entries the tool calls:

| entry | params | identical fields | differences |
|---|---|---|---|
| 0x02 `Open` | 2 | all descriptors | ext block only |
| 0x30 `Close` | 2 | all descriptors | 1 byte (strict bit) + ext block |
| 0x5E `Invoke` | 7 | 6 of 7 descriptors | 1 byte (strict bit), param 5 type offset, ext block |

Two residual classes of difference, both explained:

1. **Extension block.** It is length-prefixed (`+22` = its own size), so
   parsers skip it; its content is midl-version specific. midl 8.01 emits 8
   bytes (`08 01 00 00 00 00 00 00`); the 2011 midl emitted 12/14/18 bytes
   with an extra payload (`02 00 02 80 81 00` style) and sets ext flags2 bit
   0x40. The self-consistency holds on both sides:
   `+22 + ext_size + 6·count == entry_length` (arm: 22+18+42 = 82 ✓).
2. **`[range]` on a size parameter.** arm's type string stores the range check
   inside the correlation descriptor — `… 29 00 08 00 | 11 00 | 00 00 00 00 |
   00 00 10 00 | 02 5b` (corr flags 0x0011 + min 0 + max 0x00100000 = 8 extra
   bytes per array), whereas midl 8.01 emits `… 29 00 08 00 | 01 00 | 02 5b`.
   That is why arm's array types are 10 bytes longer, why their type offsets
   are `0x20`/`0x3a` instead of `0x20`/`0x30`, and why arm's array parameter
   lacks `IsSimpleRef`. `[strict]` context handles could not be reproduced
   either: midl 8.01 rejects `strict` in every position tried (IDL attribute
   list, ACF typedef, ACF operation attributes).

Neither residual changes a stack offset, a direction, a pointer level or the
wire layout — i.e. the parameter table above is what the format string means.

## 4. Consequence for the x86 port (why `-install`/`-remove` failed)

The session is a **20-byte context handle** (16-byte GUID + 4 attributes), not
an `int`. `src/rpc.c` stores it in an `int` (`wss_rpc_session_open(int
*pSession)`, `int session` in `wss_rpc_invoke`), so only the first dword of the
GUID survives; `proc2` then sends a corrupted handle and the server answers
`0x80070006 ERROR_INVALID_HANDLE` — which is exactly the observed failure, and
the call *did* reach the server. `proc0` likewise returns `S_OK` with
`*pSession == 0` because the server wrote 20 bytes into a 4-byte slot.

Fix direction: generate the client stub from the IDL above instead of
hand-copying the format blobs (`midl /env win32 …`) and call
`Open`/`Close`/`Invoke` directly, letting rpcrt4 allocate and marshal the
context handle; keep `RPC_C_AUTHN_LEVEL_PKT_PRIVACY` and the LOCAL SERVICE SID
from `src/rpc.c`, which are already verified on the Win8 VM.

## 5. Remaining unknown

The fourth entry (offset `0xb0`, procedure index 3, stack size 0x20, 7
parameters, Oi2 flags `0xc7`) is another `Invoke`-shaped method whose context
handle sits at stack offset 4 and which apparently uses an async handle
(`HasAsyncHandle`). `arm.exe` never calls it; it is not needed for the port.
