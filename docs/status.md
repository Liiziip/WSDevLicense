# Port status — what is verified, what is still open

## 2026-10-28 — proc2 decoded; hand-written marshalling replaced by a MIDL stub

The format entry at `0x4018EE` is decoded and verified against a midl reference
build ([ndr-proc2.md](ndr-proc2.md)).  Two findings changed the client:

* the session is a **context handle** (`FC_BIND_CONTEXT`), 20 bytes on the wire,
  not an `int` — the old code truncated it to 4 bytes, which is exactly why the
  service answered `0x80070006 ERROR_INVALID_HANDLE`;
* the request buffer is a conformant array whose conformance parameter carries
  `[range(0, 0x100000)]` — the 1 MiB cap the app enforces itself.

`src/rpc.c` now calls the stub that midl generates from `src/wss_rpc.idl`
(`tools\midl_stub.cmd` regenerates `src/wss_rpc.h` / `src/wss_rpc_c.c`), so the
context handle is allocated and marshalled by rpcrt4 instead of by hand.  The
generated format string reproduces arm.exe's entry byte for byte except the two
documented midl-version differences (extension block, `[range]`-in-correlation
encoding).

Verified on this machine (Windows 11, **no WSService**): `build\arm_x86.exe`
builds, `-check` prints the licence expiry, and `-install` / `-remove` run the
whole client path — binding, LOCAL SERVICE auth and `Open` all succeed — and
then stop at `0x800706BA RPC_S_SERVER_UNAVAILABLE`, followed by
`0x80070424` from the (absent) service start.  That is the correct result for a
machine without WSService.

**On the Windows 8.0 x86 VM the tool works end to end: `-install`, `-check` and
`-remove` all behave like the original `arm.exe`.**  With that, every stage of
the port — command line, service start/wait, ncalrpc binding, LOCAL SERVICE
authentication, the context handle lifetime, the serialised property bag and the
call itself — is confirmed against a live WSService.

Kept for the record: `ARM_X86_RPCVAR`, `ARM_X86_RPC3` and `ARM_X86_OPENSWEEP`
were the switches used while the calling convention was still unknown.  They are
gone; `src/rpc.c` no longer contains hand-written `NdrClientCall2` variants.

## Earlier results (unchanged, kept for the record)

## Verified (static + on a Windows 8.0 x86 VM)

| Area | Evidence |
|---|---|
| Build | `build\arm_x86.exe`, I386 console PE, MSVC 2019 x86, `/W3` clean |
| Command line | `-install/-remove/-check`, both `-` and `/` spellings, usage text, colour codes (0x0a/0x0c/0x0e) |
| `-check` | On Windows 11 it prints `Developer License is installed. Expiration is 9999-12-31T00:00:00Z`; on the Win8 VM it returns the expected `0x80070490` (no licence installed). `WSClient!CheckDeveloperLicense` + ISO-8601 formatting work. |
| MIDL stubs | `RpcStringBindingComposeW` (ncalrpc / WSSServiceTransportEndpoint-00001), `RpcBindingFromStringBindingW`, `RpcBindingSetAuthInfoExW` with `RPC_SECURITY_QOS_V3` + LOCAL SERVICE SID: all return 0 on the VM |
| proc0 | Reaches the server and returns `S_OK` |
| Request body | Byte-exact: three records `{crc, vt=4, nameLen=0, dataLen}`, 8-byte aligned, CRC-32 (poly 0x04C11DB7, MSB-first) verified against an independent implementation and against the x64 build's parser |

## Open — resolved (kept for the record)

`proc2` used to be rejected because the client sent a truncated handle; it now
works. The table below is the state *before* the context-handle fix, i.e. the
evidence that narrowed it down:

| call shape | result |
|---|---|
| `(&session…)` only, implicit binding slot NULL | `0x800706EF` `RPC_X_SS_HANDLES_MISMATCH` |
| `(&session…)` only, implicit binding slot set | `0x800706EF` (reproduced locally) |
| `(&binding, &session, …)` (binding as first argument) | `0x80070006` `ERROR_INVALID_HANDLE` |
| `(session, …)` by value | blocks |

`0x80070006` came back as a normal HRESULT, i.e. the call **did reach the server and was
unmarshalled**, and the server rejected one of the handles. `0x800706EF` is raised while
marshalling on the client.

### What the x64 build proved

The x64 build of the same tool **works** on the same Windows 8 x64 server (install, check,
remove, and `Get-WindowsDeveloperLicense` all agree). Therefore:

* the service and the licence-file format are fine;
* the wire format produced by `src/serialize.c` is right (also confirmed by decompiling the
  server's own record decoder, `FUN_10038EA8` + CRC helper `FUN_100391C9` in `WSService.dll`);
* the security callback `FUN_1002DC52` requires `RPC_C_AUTHN_LEVEL_PKT_PRIVACY` (6), which is
  exactly what `src/rpc.c` sets;
* the remaining difference is purely the **x86 `NdrClientCall2` argument convention for
  `proc2`**, not the protocol.

Relevant x64 evidence: its `proc0` is `NdrClientCall3(proxy, 0, NULL, binding)` (a single
argument, **no out parameter** - so the "session" the ARM build passes on to `proc2` is an
uninitialised 0 and a zero session is legitimate), and its `proc2` is
`NdrClientCall3(proxy, 2, NULL, session, opcode, reqlen, req, &resp, &resplen)`. The x64 build
uses the stubless `NdrClientCall3`, where the binding comes from the proxy and is absent from
the argument list, while the x86 format string uses `FC_BIND_PRIMITIVE` (`0x32`) - the two
conventions differ, and that difference is what is still not right.


## Where to continue

`WSService.dll` (x86, 1.9 MB, built 2012-01-28) from the VM is now in the workspace.

Found so far:

* interface `{C453C318-B4A8-4188-80C4-73E6C7417DBF}` at VA `0x1001C5FC`;
* server `RPC_SERVER_INTERFACE` at `0x1001C5F8`, `MIDL_STUB_DESC` at `0x1001C790`;
* server procedure format string base `0x1001C640`, proc0 at `0x1001C642`
  (differs from the client entry only in 6 bytes of the parameter area);
* the CRC-32 table the bag parser uses is at `0x1001F538` - its referencing function at
  `0x100391F1` is the record decoder;
* **the RPC registration function is `FUN_1002DA52`** (decompiled in
  `work/outsvc3/1002dad7.c`):

```c
RpcServerUseProtseqEpW(L"ncalrpc", 100, L"WSSServiceTransportEndpoint-00001", sd);
RpcServerRegisterIf3(&iface, NULL, NULL, 0x21 /*AUTOLISTEN|LOCAL_ONLY*/,
                     0x4d2, 0, FUN_1002dc52 /*security callback*/, sd);
```

Two concrete next steps:

1. `FUN_1002dc52` is the `RPC_IF_CALLBACK_FN` that authorises callers - decompiling it
   shows exactly which identity/SID the service requires, which is worth checking against
   what the client sends.
2. The MIDL dispatch table is not the array at `0x1001C7E0`: its entries
   (`0x1003A5C2` …) lie outside `.text` (`0x10001000`-`0x1001D285D`), so that address is
   something else.
   **However**, `NdrServerCall2` is reached through a single thunk at **`0x1008F095`**
   (`jmp dword ptr [0x100010C8]`), and four copies of that thunk address sit at
   **`0x1001C5D8`** - that is the real four-entry `RPC_DISPATCH_TABLE` (all entries share the
   generic stub, i.e. the interface uses the "NdrServerCall2 + MIDL_SERVER_INFO" style).
   The procedure implementations are therefore the functions referenced by the server info
   behind `0x1001C5B8`; re-read that structure with the corrected layout.
3. Decode the server proc0 entry at `0x1001C642` against midl-generated references
   (`tools/ndr_proc.py` is the harness that replaced `midl_probe.py`).


## Runtime switches kept in the binary

| variable | effect |
|---|---|
| `ARM_X86_TRACE=1` | per-step `[trace]` output on stderr |
| `ARM_X86_RPCVAR=0..5` | selects the argument shape used for `proc2` (see `src/rpc.c`) |
