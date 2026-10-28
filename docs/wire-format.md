# WSS request format (serialised property bag)

This is the body that `arm.exe` sends in RPC `Invoke` (opcode 0 = install, 0xc = remove).
It was determined from the ARM build (`src/serialize.c`, transcribed from `0x4b4c`, `0x4528`,
`0x4f20`, `0x40dc`) and independently confirmed twice:

* against the x86-64 build's record parser at `x64 0x1400058e4`, which hashes the first dword as
  zero and compares it with `~crc` (`1400058e4`: `if (~uVar9 == *puVar14)`), reads
  `puVar14[1]` as the type, `puVar14[2]` as the name length and `puVar14[3]` as the data length,
  and pads with `(len + 7) & ~7`;
* against a standalone re-implementation of the CRC (bitwise, poly 0x04C11DB7, MSB-first).

## Layout

Little endian. **No file header, no record count, no terminator** — the buffer length is the
only end marker. The buffer is simply a sequence of records, one per property, in bag order.

```
record :=
  +0x00  u32  crc          CRC-32 of the record, see below
  +0x04  u32  vt           property type: 1, 2, 4, 0x80000002, 0x80000004
  +0x08  u32  nameLen      bytes of the name block, 0 when the property has no name
  +0x0c  u32  dataLen      bytes of the data block (includes the NUL wchar for strings)
  +0x10  ...  name         nameLen bytes, zero padded to a multiple of 8
  ...         data         dataLen bytes, zero padded to a multiple of 8
```

* `recLen = 16 + align8(nameLen) + align8(dataLen)`, so every record starts 8-byte aligned.
* A name is a UTF-16 string including its terminating NUL, `nameLen = 2*n + 2`; `nameLen = 0`
  means "no name".
* Data by type:
  | vt | data |
  |---|---|
  | `1` | the 4-byte pointer value itself (dataLen = 4) |
  | `2` | UTF-16 string including NUL, `dataLen = 2*n + 2` |
  | `0x80000002` | array of strings, encoded like `vt = 2` |
  | `4` | raw bytes, `dataLen = cb` |
  | `0x80000004` | array of blobs, encoded like `vt = 4` |
* Padding bytes are zero and are **not** covered by the checksum.

## Checksum

CRC-32 with polynomial `0x04C11DB7`, MSB-first (the 256-entry table the binary carries at
`0x401BD0`), initial value `0xFFFFFFFF`, final XOR `0xFFFFFFFF`. Coverage, in order:

1. the 16 header bytes, **with the `crc` field itself taken as zero**;
2. the `nameLen` name bytes;
3. the `dataLen` data bytes.

Padding is excluded. The stored value is the final (already XOR-ed) CRC.

In C (`src/serialize.c`):

```c
crc = ~wss_crc32(0xFFFFFFFFu, hdr_with_zero_crc, 0x10);
crc = ~wss_crc32(~crc, name, nameLen);
crc = ~wss_crc32(~crc, data, dataLen);
```

## The install request

`wss_install_license()` adds three unnamed blob properties, in this order:

| index | vt | data |
|---|---|---|
| 0 | 4 | u32 little-endian file size (4 bytes) |
| 1 | 4 | the licence XML file bytes (`cb` = file size) |
| 2 | 4 | u32 little-endian constant `6` (4 bytes) |

For a 0x20-byte licence file the buffer is 96 bytes; the self-test
(`tests/selftest_serialize.c`, `build\selftest.exe`) prints exactly:

```
76 7f a8 8f  04 00 00 00  00 00 00 00  04 00 00 00    crc, vt=4, nameLen=0, dataLen=4
20 00 00 00  00 00 00 00                             data = 0x20, pad to 8

37 e2 79 ab  04 00 00 00  00 00 00 00  20 00 00 00    crc, vt=4, nameLen=0, dataLen=0x20
41 42 ... 46                                          the 0x20 payload bytes (no pad needed)

5b 5d ae a5  04 00 00 00  00 00 00 00  04 00 00 00    crc, vt=4, nameLen=0, dataLen=4
06 00 00 00  00 00 00 00                             data = 6, pad to 8
```

Each CRC in that dump was recomputed independently with the algorithm above and matches.

The remove request (opcode `0xc`) sends **no** body at all: the bag is `NULL`, so the RPC
layer passes a null pointer and a length of 0.

## Response body

The reply is parsed back into a bag (`0x40dc`). The parser accepts only `vt = 4` records and
rejects anything else with `0x8007000D`; a checksum mismatch is `0x80070017`, a short buffer
`0x8007000D`. The response parsing only influences the reported HRESULT, so an unexpected
reply shape is reported as a failure exactly like the original does.
