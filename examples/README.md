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
for its wiring, memory map, and measured timing. Neither host example provides
motor-control hardware shutdown; adapt the safety step to your board before
using fault logging in a machine.

For the public calls, result codes, storage-adapter requirements, and
production checks, continue with the [integration guide](../docs/integration.md)
and [`include/log.h`](../include/log.h).
