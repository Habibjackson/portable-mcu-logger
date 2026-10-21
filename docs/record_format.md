# On-storage format, version 1

All multibyte integers are **little-endian**, independent of the MCU's C
structure layout. Addresses in the backend API are byte offsets from the
device base. A page begins at the backend's erase-region start. The page header
is always 32 bytes; records follow contiguously. Erased bytes are `0xFF`.

## Page header

| Offset | Bytes | Value |
| ---: | ---: | --- |
| 0 | 4 | ASCII `ELPG` |
| 4 | 1 | Format version `1` |
| 5 | 1 | Priority/partition `0..2` |
| 6 | 1 | Header length `32` |
| 7 | 1 | Reserved zero |
| 8 | 8 | Unsigned page generation |
| 16 | 4 | CRC-32/IEEE of bytes 0..15 |
| 20 | `12-g` | Erased padding |
| `32-g` | `g` | Commit pattern `0xA5` in every byte |

`g` is the backend's program granularity (1, 2, 4, or 8 bytes for this
format). The header body is programmed and read back first; the separate final
`g` bytes are programmed last. A page with a torn or invalid header is not
treated as an erased page. A page with an erased header is reusable only if
**all** remaining bytes in the erase region are also erased.

## Record

| Offset | Bytes | Value |
| ---: | ---: | --- |
| 0 | 4 | ASCII `ELOG` |
| 4 | 1 | Format version `1` |
| 5 | 1 | Header length `36` |
| 6 | 1 | Record type (`log_types.h`) |
| 7 | 1 | Priority `0..2` |
| 8 | 1 | Codec ID |
| 9 | 1 | Flags; bit 0 means absolute time, zero means uptime |
| 10 | 2 | Reserved zero |
| 12 | 8 | Unsigned sequence |
| 20 | 8 | Timestamp or uptime supplied by caller |
| 28 | 4 | Event or fault ID |
| 32 | 2 | Original payload length |
| 34 | 2 | Stored payload length |
| 36 | `stored_length` | Encoded or original payload |
| after payload | 4 | CRC-32/IEEE of header and stored payload |
| after CRC | 0..`g-1` | Erased padding to `g` alignment |
| final `g` | `g` | Commit pattern `0xA5` |

The physical record length is
`align_up(36 + stored_length + 4, g) + g`, at most 128 bytes. The CRC uses the
reflected IEEE polynomial `0xEDB88320`, initial state `0xFFFFFFFF`, and final
XOR `0xFFFFFFFF`. The commit unit is separate and aligned. It is programmed
only after the body has been written and verified. A committed record still
needs valid lengths, header, CRC, erased padding, priority matching its page,
and complete commit bytes. CRC detects corruption but does not make writes
atomic. Unknown format versions return `ELOG_UNSUPPORTED` in firmware.
The current high-level write API emits uptime flag zero; applications should
use a monotonic timestamp provider.

Codec IDs: `0=NONE`, `1=DELTA`, `2=VARINT`, `3=BLOCK` (reserved), and
`128..255` for application codecs. The built-in numeric codecs take signed
32-bit little-endian words. `VARINT` starts with a one-byte word count, then
ZigZag LEB128 words. `DELTA` starts with the 8-byte base record sequence and a
one-byte word count, then ZigZag LEB128 signed differences. A decoder must
find the validated base record before applying a delta. When compression
expands, the logger stores `NONE` with equal original/stored lengths.

Sequence and generation comparison uses modulo 2^64 with a half-range rule:
distance exactly 2^63 is ambiguous and rejected. Record iteration yields
physical order, not sequence order. `tools/decode_logs.py` validates and sorts
known version-1 records by sequence, and reports undecodable custom codecs.
