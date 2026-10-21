# Recovery and power-failure behavior

The journal programs each page header body, verifies it, then programs its
commit unit. It follows the same sequence for every record body and commit.
A record is durable only after its commit has been read back with a valid CRC.
The final commit is a separate aligned program unit; the core never tries to
reprogram a torn unit. `elog_init` and `elog_recover` scan read-only. A damaged
or uncommitted record tail seals the rest of that page, preserving its earlier
valid records. The next write chooses another erased page or, for rotating
lanes, safely erases an eligible older page. Erases run only when `may_erase`
allows them; the application must separately gate internal-flash programming.

An erased page header alone is insufficient evidence of a blank region. A
power cut during erase can clear the header while old body bytes remain. The
scanner checks the **whole** region before classifying it as erased. It also
checks that bytes after an erased record gap are erased before appending. A
partly erased region is marked torn and is erased as a whole before reuse.

For EVENT and ROUTINE, the logger starts a committed header in another page
before reclaiming the oldest non-active page. Their retention is circular;
records on a reclaimed page are intentionally lost. CRITICAL never erases a
committed page automatically. Once full, failed critical records are reported
and counted, while other lanes can still make progress. The application must
export the required records and perform an explicit maintenance policy before
reusing critical capacity. A backend error stops the current operation; call
`elog_recover` or reboot after diagnosing it. A commit may have completed even
if the backend returned a timeout, so recovery checks storage and avoids a
duplicate when it can identify that pending record.

The fault-injection suite cuts page-header body/commit, record body/commit,
and page erase at several byte positions; it also corrupts a CRC and checks
unsupported format versions. These tests validate the software state machine
under the simulator. They do not model supply brownout thresholds, charge
retention, interrupted HAL code fetches, incomplete SPI transactions, or the
device's actual power-loss guarantees. CRC is error detection, not atomicity.
Physical board tests with controlled power cuts and a known hold-up budget are
required before claiming fault-log durability under power loss.

An ISR fault capture is RAM-only until task processing commits it. Queue
overflow, reset before commit, storage errors, and a full critical partition
can all lose that pending fault; the API and statistics expose these outcomes
where software can observe them. The active-fault suppression table and
pre/post telemetry windows are not persisted across reboot. Unknown format
versions return `ELOG_UNSUPPORTED` so data from a future format is not silently
interpreted as version 1. Sequence and page generations use modular 64-bit
ordering; an exact half-range ambiguity returns corruption.
