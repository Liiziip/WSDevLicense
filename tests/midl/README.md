# Midl experiments from the format-string investigation

These are the reference stubs generated while working out what the procedure format strings
in `arm.exe` mean.  They are kept as evidence: `docs/ndr-stage1.md` cites them.

| file | what it is |
|---|---|
| `explicit.idl`, `explicit.acf` | `HRESULT CreateSession([out] int *session)` with `explicit_handle` — the first candidate whose format bytes matched the head of the `0x401892` entry |
| `explicit_c.c` | midl's client stub for the above, with its own comments naming every format byte |

The current, complete reference stub lives in `src/wss_rpc.idl` / `src/wss_rpc.h` /
`src/wss_rpc_c.c`, and the tool that regenerates and diffs it is `tools/ndr_proc.py`
(`python tools\ndr_proc.py verify`).
