# arm.exe — analysis notes

`arm.exe` (27,136 bytes, PE32, machine `0x01C4` ARMNT / ARMv7 Thumb-2, console subsystem,
image base 0x400000, link timestamp 2011-11-18) is the Microsoft internal
*"Utility for Windows Store Developer Licensing"* used on Windows RT to install, remove and
inspect the per-machine developer licence that allows sideloading UAP/Store apps.

```
arm.exe -install <license.xml>    install the developer licence
arm.exe -remove                   remove it
arm.exe -check                    print the expiry (UTC, ISO-8601)
```
Both `-x` and `/x` spellings are accepted; anything else prints the usage block.

## Structure

| section | VA | vsize | content |
|---|---|---|---|
| `.text` | `0x1000` | `0x5654` | IAT at `0x1000`..`0x1148`, code and constant data (`.rdata` was merged into `.text`) |
| `.data` | `0x7000` | `0x374` | globals (COM vtable slots, lock words) |
| `.pdata` | `0x8000` | `0x2b8` | ARM unwind data |
| `.reloc` | `0x9000` | `0x74e` | base relocations |

Imports: `KERNEL32`, `msvcrt` (static-ish CRT startup, `__wgetmainargs`, `vwprintf`),
`WSClient.dll!CheckDeveloperLicense`, `RPCRT4` (`NdrClientCall2`, `RpcStringBindingComposeW`,
`RpcBindingFromStringBindingW`, `RpcBindingSetAuthInfoExW`, `I_RpcMapWin32Status`),
`ADVAPI32` (SCM: `OpenSCManagerW`, `OpenServiceW`, `StartServiceW`, `QueryServiceStatusEx`,
`NotifyServiceStatusChangeW`, `AllocateAndInitializeSid`).

Ghidra recovered **106 functions**, all of which decompile (that workshop is not part of
this repository and nothing below needs it). Only ~35 of them are the
application; the rest are MSVC CRT startup/`__chkstk` stubs and the MIDL-generated
serialisation helpers.

## Application layer (addresses are ARM VAs)

| VA | role |
|---|---|
| `0x50dc` | `mainCRTStartup` — runs initialisers, calls `main(0x22d0)`, exits |
| `0x22d0` | `main`: argument dispatch, success/usage output, cleanup |
| `0x25d8` | read the whole licence file (`CreateFileW`/`GetFileSize`/`HeapAlloc`/`ReadFile`) |
| `0x2be0` | open session, and on failure start `WSService` and retry once |
| `0x2990` | RPC proc #0: create session (creates the ncalrpc binding first, `0x2774`) |
| `0x2a3c` | RPC proc #1: close session |
| `0x2ac4` | RPC proc #2: request/response round trip (opcode, blob in, blob out) |
| `0x2c2c` | build the install request (3 properties) and invoke with opcode 0 |
| `0x2d14` | open SCM + `WSService`, start it if needed |
| `0x2e70` | wait for RUNNING via `NotifyServiceStatusChangeW` + `SleepEx(30000)` |
| `0x2f3c` | poll RUNNING, 30 × 1 s |
| `0x21c0` | coloured `vwprintf`: set console attribute, print, restore |
| `0x2238` | error report via `FormatMessageW`, plus the "run elevated" hint for `ERROR_ACCESS_DENIED` |
| `0x2538` | "check expected HRESULT" (no-op unless a global trap value is armed) |
| `0x2554` | failure-reporting hook (logging only) |

## RPC interface

```
transport   ncalrpc
endpoint    WSSServiceTransportEndpoint-00001
principal   NT AUTHORITY\LOCALSERVICE          (pre-Win8: as a name; Win8+: a LOCAL SERVICE SID)
authn       6 = RPC_C_AUTHN_LEVEL_PKT_PRIVACY, 10 = RPC_C_AUTHN_WINNT
interface   {C453C318-B4A8-4188-80C4-73E6C7417DBF}  v1.0
syntax      {8A885D04-1CEB-11C9-9FE8-08002B104860}  (NDR32) v2.0
```

The client uses **explicit binding handles**. Three procedures are referenced through the
MIDL format-string table at `0x401890` (offsets `0x02`, `0x30`, `0x5e`) with the stub
descriptor at `0x401A80` and the type format string at `0x40199A`; the blobs are copied
verbatim into `src/midl_blob.c` for forensics.  The interface was decoded from them and is
now rebuilt as `src/wss_rpc.idl`, so the calls go through a midl-generated stub whose
format string matches the `0x4018EE` entry byte for byte (see `ndr-proc2.md`).

* `proc 0` — `HRESULT Open([out] WSSCTX *pctx)` with an explicit primitive binding; the
  out parameter is a **context handle**, which supplies the binding for the other calls
* `proc 1` — `HRESULT Close([in,out] WSSCTX *pctx)`
* `proc 2` — `HRESULT Invoke([in] WSSCTX ctx, [in] OP opcode,
  [in, range(0,0x100000)] unsigned long cb, [in, size_is(cb)] byte p[],
  [out] long *pcb, [out, size_is(,*pcb)] byte **pp)`

Opcodes seen: `0` = install, `0xc` = remove. A request larger than 1 MiB is rejected with
`0x8007006F` before the call.

The request body is a serialised property bag (see `wire-format.md`). Install sends
properties `0` = 4-byte length, `1` = the licence XML, `2` = the constant `6`.

## Cross-check against the x64 build

`x64.exe` (32,256 bytes, AMD64, 2011-08-06) is an older build of the same tool. It uses
`NdrClientCall3` + a stubless proxy and polls the service instead of using
`NotifyServiceStatusChangeW`, but the interface UUID, the endpoint strings and the proc
format strings are byte-identical, and the command flow (including the three install
properties and opcode `0xc` for remove) is the same. It was used only to validate structure.

## Reconstruction deviations (documented on purpose)

1. `WSClient!CheckDeveloperLicense` is resolved with `LoadLibraryW`/`GetProcAddress` instead
   of a static import, because the import library is not available in this build
   environment. Behaviour is the same.
2. `wmain` returns 0 on success and 1 on failure; the original `main` is `void`, so its exit
   code was whatever happened to be in `r0`.
3. The failure-reporting hook (`0x2554`) is compiled out (it only fed an internal ETW/log
   path), and the "expected HRESULT" trap from `0x2538` only fires if `g_wssThrowHr` is set.
4. `WSService` start/wait is expressed with the same API calls but a simpler control flow
   than the ARM original's duplicated state machine; the observable results (service started,
   `HRESULT_FROM_WIN32(ERROR_TIMEOUT)` after ~30 s) are the same.
