#Cautious!!  This project is completely reversed by AI and I have completely no ideal what it had written in the code.

# arm.exe → x86 reconstruction

A source-level port of `arm.exe`, the Windows RT *"Utility for Windows Store Developer
Licensing"* (27,136 bytes, ARMv7 Thumb-2 PE32, console, built 2011-11-18), rebuilt as a
32-bit x86 Win32 console program.

The point of the project is less "port a 27 KB tool" than "recover an undocumented RPC
interface from a stripped ARM binary": the NDR stub data was decoded by hand, the interface
was regenerated with midl, and every byte of the procedure format entry was reconciled
against the reference stub.  `docs/` records that work, including the dead ends.

## Status

**Complete.**  On a Windows 8.0 x86 VM `-install`, `-check` and `-remove` all behave like the
original.  On a machine without `WSService` (stock Windows 11, for instance) the tool reports
the same failure the original would: `0x800706BA` from the call, then `0x80070424` when the
service cannot be started.

| area | evidence |
|---|---|
| RPC interface, wire format | decoded from the NDR format strings at `0x401890`/`0x40199A`, rebuilt as `src/wss_rpc.idl`, verified against a midl reference stub — `docs/ndr-proc2.md` |
| serialised licence request | byte-exact property bag, CRC-32 checked against the server's own decoder — `docs/wire-format.md` |
| end to end | `-install` / `-check` / `-remove` on Windows 8.0 x86 — `docs/status.md` |

Two findings mattered most, and both are in `docs/ndr-proc2.md`: the session is a **20-byte
context handle** (`FC_BIND_CONTEXT`), not an `int`, and the request buffer is a conformant
array whose size parameter carries `[range(0, 0x100000)]`.

## Layout

```
arm_x86.sln            Visual Studio 2019 solution (Win32 / x86 only)
arm_x86.vcxproj        the tool            -> build\arm_x86.exe
build.cmd              command-line equivalent (vcvars32 + cl)
src/                   the reconstructed sources
  wss.h                shared types (WssValue, WssBag, WssWriter, HRESULT plumbing)
  main.c               wmain: -install / -remove / -check / usage
  license.c            install / remove / check operations, session + service retry
  wss_rpc.idl          the WSS interface, reconstructed from the NDR format strings
  wss_rpc.acf          ACF (explicit_handle)
  wss_rpc.h/.c         midl-generated client stub (tools\midl_stub.cmd regenerates)
  rpc.c                ncalrpc binding + LOCAL SERVICE auth + Open/Close/Invoke
  midl_blob.c/.h       original format strings + RPC_CLIENT_INTERFACE, copied verbatim
                       from arm.exe (forensics; no longer used for calls)
  bag.c                property bag object (ARM 0x31d0..0x3ec0)
  serialize.c          bag -> bytes / bytes -> bag (ARM 0x3ff0..0x5030)
  svc.c                WSService start + wait
  file.c               whole file reader
  print.c              coloured console output, error reporting
tests/
  selftest.vcxproj     the selftest project -> build\probes\selftest.exe
  selftest_serialize.c in-memory check of the serialised install request
  midl/                the early midl experiments referenced by docs/ndr-stage1.md
docs/
  analysis.md          binary structure, function map, RPC interface, deviations
  ndr-proc2.md         decoding + verification of the proc2 format entry (0x4018EE)
  ndr-verify.txt       frozen output of `tools\ndr_proc.py verify`
  ndr-stage1.md        the (superseded) plan that started the format-string work
  wire-format.md       the serialised request format
  status.md            verification log, including what was disproved
  debug-howto.md       how to run this under a debugger
tools/
  ndr_proc.py          MIDL/arm.exe format-string oracle: decode, compare, verify
  mkblobs.py           regenerate src/midl_blob.c from arm.exe
  pedump.py            minimal PE reader used by the scripts
  dump.py              hex-dump a VA range of a PE (the format strings, GUIDs, ...)
  midl_stub.cmd        regenerate src/wss_rpc.h/.c from the IDL
```

## Build

Open `arm_x86.sln` in Visual Studio 2019 and press F7 (or F5 to debug).  The
solution has two Win32-only projects:

| project | output | what it is |
|---|---|---|
| `arm_x86` | `build\arm_x86.exe` | the tool |
| `selftest` | `build\probes\selftest.exe` | serialiser round-trip check, run it from the console |

The vcxproj carries the same settings the original command line used: compile as C (`/TC`),
`/W3`, `/GS-`, console subsystem, `rpcrt4.lib` + `advapi32.lib`, and `build\` as the output
directory so the paths in `docs/` stay valid.

Command line equivalent, if you prefer not to open the IDE:

```
build.cmd
```

It uses `%VSDIR%\VC\Auxiliary\Build\vcvars32.bat` (default
`D:\Microsoft Visual Studio\2019\Professional`, override with the `VSDIR` environment
variable) and produces the same `build\arm_x86.exe`.

Either way the generated stub (`src/wss_rpc_c.c`) is checked in, so building needs neither
midl nor the original binaries.  Run `tools\midl_stub.cmd` only if `src/wss_rpc.idl`
changes.

## Runtime behaviour

Same command line as the original:

```
arm_x86.exe -install <license.xml>
arm_x86.exe -remove
arm_x86.exe -check
```

`-install` / `-remove` go through the ncalrpc endpoint `WSSServiceTransportEndpoint-00001`
with `RPC_C_AUTHN_LEVEL_PKT_PRIVACY` and LOCAL SERVICE impersonation, exactly like the
original, so they only work where `WSService` exists (Windows 8.x / 10).
`ARM_X86_TRACE=1` prints a per-step trace on stderr.

## Verification

`tools/ndr_proc.py` re-derives the whole interface from scratch: it builds a reference stub
with midl for the reconstructed IDL and diffs it against the bytes in `arm.exe`:

```
python tools\ndr_proc.py arm        # dump the two format strings from arm.exe
python tools\ndr_proc.py verify     # build the reference stub and diff every field
```

`verify` writes `docs/ndr-verify.txt`.  It reports that 6 of the 7 parameter descriptors of
`proc2` are byte-identical, and that the remaining differences are two documented
midl-version details (the length-prefixed extension block, and how midl 7 encoded a `[range]`
check inside a correlation descriptor).

## Provenance

This is a reverse-engineering exercise for interoperability and preservation: the program
and its RPC interface exist only as a stripped 2011 ARM binary that no longer runs on any
supported OS.  No Microsoft binaries, symbols or SDK source are redistributed here — only
observations, byte-level facts and newly written code.  Reviewed against the original where
possible: undocumented behaviour is marked as such in `docs/` rather than guessed.
