# Architecture and API contract

## Data flow

`elog_write` copies a caller's payload into one bounded priority queue. The
task calls `elog_process` to select the highest pending priority, encode a
record, select an erase region, and advance one journal state. Each state can
perform one synchronous storage call, so an individual call may still block for
one program or erase operation. `elog_flush(logger, max_steps)` repeats that
work up to `max_steps` times. `elog_init` scans existing storage and never
formats it. The caller allocates `elog_t`, the storage adapter, and all storage
buffers for the whole logger lifetime; there is no heap allocation in the
library.

The storage interface uses device-relative offsets and exposes capacity,
program granularity, maximum program size, the erase region containing an
offset, read, program, erase, busy, and sync. The core accepts variable erase
region sizes, but all three configured partitions must start and end on whole
erase regions. Backends are synchronous and verify writes before returning
success. The core checks the body, commit, CRC, and page header again after
programming. EEPROM or FRAM can be added through the same interface if their
backend provides erased-unit semantics compatible with the journal.

## Priority and retention

| Lane | Queue overflow | Flash retention | Ordering |
| --- | --- | --- | --- |
| CRITICAL | Reject new entry, count loss | Reserved pages; no automatic erase | First |
| EVENT | Reject new entry, count loss | Rotate oldest non-active page | Second |
| ROUTINE | Replace oldest queued entry, count loss | Rotate oldest non-active page | Third |

`elog_process` chooses ISR faults, then task critical entries, then events,
then frozen pre/post fault telemetry, then routine telemetry. A flash operation
already underway cannot be preempted. The three physical partitions ensure
that routine and event data cannot consume critical flash space. At critical
capacity exhaustion, processing returns `ELOG_STORAGE_FULL`, counts the failed
pending record in `dropped_storage`, and continues servicing other lanes on
later calls. New task critical writes then return `ELOG_STORAGE_FULL` until
recovery or service. ISR capture remains bounded; if it queues a fault when
critical storage is full, the task reports the persistence failure.

The active-fault table suppresses repeated reports for the same ID. A repeat
summary can be sent after `fault_repeat_interval_ms`; a recovery record stores
the repeat count and duration before the caller's optional payload. The table
is RAM state, so active-fault suppression restarts after a reboot. Enqueue
success does not guarantee that any fault was committed. Applications must
watch processing errors and counters.

Routine telemetry up to 24 bytes can also be kept in a bounded pre-fault ring.
On first fault, its recent samples are frozen and later written as
`PRE_FAULT` records; an optional post-fault window captures later samples.
These records occupy routine storage and can be rotated away. Samples larger
than 24 bytes remain ordinary telemetry only. A post-fault window with too few
subsequent samples stays frozen until recovery or its configured count arrives.

## Configuration and concurrency

`include/log_config.h` sets compile-time maxima: 80-byte payloads, 128-byte
physical records, 64 erase regions, queue and fault-table caps, and telemetry
window caps. `elog_init` validates runtime depths, partition containment and
alignment, minimum erase region size, program granularity/max size, codec
settings, required CRC/recovery flags, and callback requirements. Config and
codec callbacks must remain valid after initialization. `timestamp` should
return monotonic 64-bit time in the same units as configured intervals.

Ordinary API calls, including status, recovery, and iteration, must be
serialized by the application in one task or protected by an application
mutex. `elog_iterate_records` requires a quiescent logger and visits physical
partition order; use the 64-bit sequence to reconstruct chronological order.
The only ISR API is `elog_capture_fault_isr`, with **one designated ISR
producer** and the task as consumer. Its C11 atomic queue must be lock-free on
the target (`elog_init` checks this). It copies at most `max_payload` bytes,
uses the caller-supplied timestamp, and does no flash I/O, compression, clock
callback, or dynamic allocation. Multiple ISR producers need an application
adapter or a different queue. The application must keep motor safety response
independent of logger success.

`sizeof(elog_t)` is 8,344 bytes on the current MinGW host build. The
PlatformIO RAM example currently reports 10,472 bytes of global RAM use,
including its 2 KiB RAM image and HAL state; this is not a full robot memory
budget. Runtime queue depths do not reduce the compile-time context size.

## Codec rules

`NONE` is the default. Built-in `VARINT` encodes signed little-endian 32-bit
telemetry words; `DELTA` stores per-word differences from the prior matching
telemetry record, with a base sequence number. The first sample after boot or
page change and periodic samples are full snapshots. Encoded data is used only
when shorter than the original payload. Custom codecs use IDs 128..255 and
must encode each record independently so rotation cannot strand a base record.
Critical records bypass compression. `BLOCK` is reserved and rejected by
configuration until a bounded implementation and matching decoder exist.
