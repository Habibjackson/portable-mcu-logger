# Examples and common usage

These host programs use byte arrays that model flash. They build on Windows,
Linux, or macOS with CMake and a C11 compiler. They do not program the Blue
Pill or alter its saved logs.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run an executable as `./build/elog_quickstart` on Unix-like systems or
`./build/elog_quickstart.exe` on Windows. The other executable names are below.

| Program | Read its source for | Typical output |
| --- | --- | --- |
| [`elog_quickstart`](quickstart/main.c) | RAM backend setup; partition geometry; timestamped routine and event writes; commit and readback | Two records; event gets sequence 0 before pending routine data |
| [`elog_reboot_recovery`](reboot_recovery/main.c) | Simulated partial write; reboot without formatting; committed-record recovery and subsequent logging | Event 1001 survives, uncommitted 1002 is absent, event 1003 commits after reboot |
| [`elog_isr_safe_state`](isr_safe_state/main.c) | Shut outputs down before capturing a fault from one ISR producer; report a full ingress queue; commit from a task | Two fault captures commit, one is rejected, safe state remains set |
| [`elog_fault_window`](fault_window/main.c) | Sample sensors every 100 ms; persist telemetry only from fault onset through recovery | Ten healthy samples cause zero writes; one fault and four sensor samples commit; later healthy samples cause zero new writes |
| [`elog_pretrigger_capture`](pretrigger_capture/main.c) | Retain recent healthy readings in an application RAM ring and save them only when a fault occurs | Six healthy readings cause zero flash writes; four earlier and three fault-window readings are retained |
| [`elog_rate_limited_telemetry`](rate_limited_telemetry/main.c) | Set a routine interval; distinguish intentional skips from queue drops; let an event bypass the routine limit | Ten attempts produce four routine records, six rate-limit skips, and one event |
| [`elog_compression_modes`](compression_modes/main.c) | Compare raw, VARINT, and DELTA storage for identical signed sensor words; decode each committed record | 72 raw payload bytes become 33 with VARINT or 63 with DELTA and frequent snapshots |
| [`elog_custom_rle_codec`](custom_rle_codec/main.c) | Register custom codec ID 128, decode a run-length payload, and inspect raw fallback | Repeated telemetry uses 7 of 24 payload bytes; mixed telemetry and an event stay raw |
| [`elog_host_demo`](host_demo/main.c) | Larger telemetry/fault session with DELTA compression and raw-image export | Writes `build/demo_logs.bin` |

## First record: `elog_quickstart`

The quickstart creates one 256-byte critical page and two pages each for event
and routine data. The event and routine lanes need at least two erase pages so
they can rotate. After initializing the RAM backend, it formats this **new
fixture once**. It then initializes the logger, queues a telemetry record and
an event, calls `elog_flush`, and visits the committed records.

`elog_write` and `elog_event` copy payloads into bounded RAM queues. An
`ELOG_OK` return from those calls does **not** prove a durable write. In a real
application, call `elog_process` from a serialized task and check its result;
`elog_flush` is convenient when that task may block until the queue drains.
The example's `elog_iterate_records` visitor reads `record.payload` only inside
the callback because the pointer borrows logger scratch memory. Physical
iteration follows partition and page order, so use `record.sequence` for
chronological order. The example uses `ELOG_CODEC_NONE`, so its eight telemetry
payload bytes can be interpreted directly as two little-endian `uint32_t`
values.

## Reboot after an interrupted write: `elog_reboot_recovery`

The reboot example wraps the RAM backend in the simulated-flash adapter. It
commits event 1001, injects a timeout after ten bytes of event 1002's body,
then creates a new logger instance over the **same bytes**. Startup scans the
journal without formatting or erasing it: event 1001 is visible, event 1002
was never committed, and a new event 1003 can be committed on the next event
page. This models one cut point, not real electrical power-loss behavior. The
full host fault-injection suite covers more cut points; real hardware still
needs controlled power tests.

The version field of a partially written record must be readable for the
scanner to classify it as a torn version-1 record. If only the record magic is
present, the scanner returns `ELOG_UNSUPPORTED` rather than guessing whether
it is a future format. That is why this example cuts after ten bytes.

## Fault in an interrupt: `elog_isr_safe_state`

The host example calls an ISR-shaped function directly. In firmware,
`disable_outputs_now` must actually put the actuator into a safe state **before**
calling `elog_capture_fault_isr`. That API only copies bytes and an explicit
timestamp into a bounded ingress queue. It does not write flash. The third
capture in this example returns `ELOG_QUEUE_FULL`, but the simulated output
shutdown has already happened. A task later calls `elog_flush`, checks the
result, and inspects committed records and the drop counter.

Only one designated ISR producer may use this ingress queue. Keep ordinary
logger calls serialized in one task; do not call the task-side APIs from other
ISRs or concurrent tasks without application synchronization. Internal flash
program and erase operations can delay interrupts, so run `elog_process` only
when the application's timing and safety policy permits them.

## Save sensor data only during a fault: `elog_fault_window`

This host example keeps sampling on a 100 ms schedule, but sends no healthy
samples to `elog_write`. On the first fault sample it queues a critical fault
record, followed by that sensor reading. It records each later sample while
the fault remains active, then one recovery-boundary sample. After the queues
drain, further healthy samples do not program or erase flash. The test checks
the retained RPM and pressure bytes against the values sampled at each fault
timestamp, as well as the flash program-call count. Run
`./build/elog_fault_window.exe` on Windows or `./build/elog_fault_window` on
Unix-like systems.

The example deliberately omits pre-fault history. No application can retrieve
sensor readings it neither retained in RAM nor committed before power loss.
Queued fault samples are also vulnerable until their flash commits finish.
For a real actuator, enter its safe state before logging a fault and schedule
flash work around the control loop's deadlines.

## Keep optional context in RAM: `elog_pretrigger_capture`

This example is useful when a fault needs a few sensor readings from just
before it started, but healthy operation must not write flash. It takes six
healthy readings, retaining the most recent four in an application-owned RAM
ring. At the fault trigger it enters a safe state, queues the critical fault
first, then queues those four readings from oldest to newest. It also commits
the first fault reading, a later fault reading, and a recovery-boundary reading.
The test verifies that healthy sampling caused zero simulated flash writes and
that all seven retained sensor payloads have the expected values.

Each sensor payload is 16 little-endian bytes: `uint64_t sample_ms`,
`uint32_t rpm`, `uint32_t pressure_deci_kpa`. The record header of a buffered
pre-trigger reading has the **trigger/enqueue time** (700 ms in the example);
the payload has its original sample time (300–600 ms). A decoder for this
application must use that payload field to reconstruct the earlier timeline.
The RAM ring disappears on reset or power loss. Queued records also disappear
unless flash processing finishes. Size the ring and logger queue for the
maximum context needed; a full routine queue can replace older pending data.

Run `./build/elog_pretrigger_capture.exe` on Windows or
`./build/elog_pretrigger_capture` on Unix-like systems. This application-owned
ring is separate from the library's built-in `pre_fault_depth` feature; this
example sets that depth to zero by leaving the zero-initialized field unset.

## Reduce routine records: `elog_rate_limited_telemetry`

This example sets `routine_interval_ms = 250` and attempts one routine write
every 100 ms. Writes at 100, 400, 700, and 1000 ms are accepted; the six
other attempts return `ELOG_WOULD_BLOCK` and increment
`statistics.routine_rate_limited`. Those skips are intentional and do not
increment `dropped_queue`. A state-change event at 500 ms uses the event lane
and commits before the four pending routine records. The example checks the
record payloads, sequence numbers, queue occupancy, and session counters.

Run `./build/elog_rate_limited_telemetry.exe` on Windows or
`./build/elog_rate_limited_telemetry` on Unix-like systems. The routine
interval reduces saved readings; it does not preserve the skipped values in
RAM. `ELOG_WOULD_BLOCK` from this routine enqueue means rate limiting, while
the same result from `elog_process` can mean storage or erase must wait.

## Compare built-in compression modes

Run `./build/elog_compression_modes.exe` on Windows or
`./build/elog_compression_modes` on Unix-like systems. The program formats a
fresh RAM-backed image for each of `NONE`, `VARINT`, and `DELTA`, then commits
the **same three** six-field samples to each image. Each field is a signed
little-endian 32-bit integer. It copies the stored payload during readback,
sorts by `record.sequence`, dispatches on the stored `record.codec_id`, and
compares all decoded bytes with the original sample.

`NONE` stores 72 of 72 payload bytes. `VARINT` stores 33 of 72 because the
readings have small ZigZag-encoded values. `DELTA` stores 63 of 72: its first
sample is raw, its second stores small differences plus an 8-byte base
sequence, and `snapshot_interval = 1` makes its third sample raw again. A
different snapshot interval or larger field values changes the savings. On a
new routine page, DELTA can also need a raw snapshot; an unavailable prior
sample cannot be decoded as a DELTA base. The logger chooses raw `NONE` when
compression does not make an individual payload shorter.

These are **payload-only** byte counts. Flash records also include headers,
CRC, commit bytes, padding, and page headers. For short records, that overhead
can outweigh the payload saving. Compression is attempted only for routine
telemetry, not event or critical records. `ELOG_CODEC_BLOCK` is reserved and
cannot be selected as an implemented compressor.

## Add a custom run-length codec

Run `./build/elog_custom_rle_codec.exe` on Windows or
`./build/elog_custom_rle_codec` on Unix-like systems. The example registers
`elog_codec_t` with ID 128. Its per-record format is one original-length byte
followed by `[run length, value]` byte pairs. Three eight-byte runs compress a
24-byte telemetry payload to seven bytes. A different-value-every-byte sample
returns `ELOG_TOO_LARGE` from the encoder, causing a raw `NONE` record. A
repeated-byte `EVENT` also stays raw because events are ineligible for routine
telemetry compression. Readback calls the matching custom decoder for ID 128
and checks the reconstructed bytes.

Persisted custom codec IDs need stable format definitions and a matching
decoder in every export tool that reads them. The supplied Python decoder does
not know this example's ID 128; the C example decodes it itself. Avoid changing
the meaning of an ID after devices have written records with it. Keep custom
callbacks bounded in time and memory, and test malformed input and power-loss
recovery for a production codec.

## Larger telemetry session and decoding

The original host demo writes a 32 KiB image with telemetry, a fault, an event,
and recovery. It uses DELTA compression. The decoder validates committed
records and turns the example event IDs and payloads into named values:

```sh
./build/elog_host_demo build/demo_logs.bin
python tools/decode_logs.py build/demo_logs.bin --page-size 2048 --granularity 2 --schema examples/robot_schema.json --human --strict
```

Use `./build/elog_host_demo.exe` on Windows. Omit `--human` for JSON output. The
schema in [`robot_schema.json`](robot_schema.json) belongs to the application:
keep its IDs, fields, scaling, and version in step with firmware payloads.

## MCU integration

[`src/main.c`](../src/main.c) is a PlatformIO STM32F103RD compile example with
volatile RAM storage. [`src/bluepill_main.c`](../src/bluepill_main.c) is the
separate C8 hardware-test application with a linker-reserved internal-flash
region and UART commands. See the [Blue Pill hardware guide](../docs/bluepill_hardware_test.md)
for its wiring, memory map, and measured timing. The host examples only model
safe-state signals; adapt the safety step to your board before using fault
logging in a machine.

For the public calls, result codes, storage-adapter requirements, and
production checks, continue with the [integration guide](../docs/integration.md)
and [`include/log.h`](../include/log.h).
