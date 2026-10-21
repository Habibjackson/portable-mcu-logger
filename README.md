# Portable embedded logger

A bounded C11 logging library for microcontrollers. The core has no HAL, RTOS,
heap, or bus dependency. It stores versioned, CRC checked records in separate
critical, event, and routine partitions and can use RAM, STM32F103 internal
flash, or descriptor driven SPI NOR storage.

## Build and test

Host build (CMake, a C11 compiler, and optionally Python 3):

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

The host demo writes a raw 32 KiB image, which the decoder can read:

```sh
./build/elog_host_demo build/demo_logs.bin
python tools/decode_logs.py build/demo_logs.bin --page-size 2048 --granularity 2
```

For smaller examples, run `./build/elog_quickstart`,
`./build/elog_reboot_recovery`, and `./build/elog_isr_safe_state` (append `.exe` on
Windows). They show first setup, reboot recovery after a simulated torn write,
and safe-state fault capture from an ISR. See the
[examples guide](examples/README.md) for commands, expected output, and usage
notes.

PlatformIO target build:

```sh
pio run -e genericSTM32F103RD
pio run -e bluepill_f103c8
```

`src/main.c` is a compile example with placeholder robot signals and a **RAM
backend**. It does not write the reserved internal flash or claim that motor
control can continue safely during flash operations. The project linker script
reserves an illustrative 32 KiB region at `0x08058000` and a post-link ELF
check rejects firmware overlap. Confirm the real board's bootloader, update,
configuration, and calibration layout before any upload or internal flash use.

The separate `bluepill_f103c8` target uses the STM32F103C8T6's nominal 64 KiB
flash and 20 KiB SRAM. It reserves the final 8 KiB of flash for a synthetic
hardware-test logger and reports status over USART1. See the
[Blue Pill hardware test guide](docs/bluepill_hardware_test.md) for its memory
map, wiring, commands, export steps, and physical test checklist.

To turn the host demo's raw numbers into an example event timeline:

```sh
python tools/decode_logs.py build/demo_logs.bin --page-size 2048 --granularity 2 --schema examples/robot_schema.json --human
```

## Using the library

1. Initialize a storage adapter and its device-relative erase geometry.
2. Set non-overlapping `config.partition[ELOG_CRITICAL/ELOG_EVENT/ELOG_ROUTINE]`
   ranges to whole erase regions. Set queue depths, maximum payload, timeout,
   and `recover_on_init = require_crc = 1`.
3. Keep an `elog_t` instance alive for the logger lifetime. Call
   `elog_init(&logger, &config, &storage)`; it scans existing records without
   formatting or erasing the device.
4. Call `elog_write`, `elog_event`, or `elog_fault` from one serialized task
   context. `elog_capture_fault_isr` is the one designated ISR producer entry
   point. Call `elog_process` from a task or a safe-state worker to advance one
   journal step; `elog_flush` runs a bounded number of steps.
5. Check every return value and `elog_get_statistics`. Queue acceptance means
   the payload was copied into RAM, not that it is durable. It becomes durable
   only after its record commit is verified.

The API is in [include/log.h](include/log.h). Start with the
[quickstart](examples/quickstart/main.c), then see the
[examples guide](examples/README.md) for recovery, ISR, host export, and
PlatformIO applications.

## Current behavior and limits

- Critical records use reserved pages and are never automatically erased.
  When those pages fill, new critical persistence reports `ELOG_STORAGE_FULL`;
  the failed pending record is counted in `dropped_storage`. The application
  must arrange export and an explicit maintenance/reformat policy before reuse.
- Event and routine pages rotate, erasing their oldest non-active page when a
  safe erase callback permits it. The application should size retention for
  the actual data rate and device endurance.
- Routine queue overflow replaces the oldest queued routine entry and reports
  `ELOG_QUEUED_WITH_DROP`. Critical and event queue overflow reject the new
  entry. The ISR ingress queue is bounded and returns `ELOG_QUEUE_FULL`.
- Built-in codecs are `NONE`, `VARINT`, and `DELTA` for little-endian signed
  32-bit telemetry. Custom independent per-record codecs can use IDs 128..255.
  `BLOCK` is reserved but has no implementation. Compression falls back to the
  original payload if it does not shrink.
- Ordinary calls need application serialization. The ISR ingress is single
  producer, single consumer and requires lock-free C11 atomics. No ISR path
  performs flash I/O. The STM32F103C8 HAL adapter has been exercised on a
  Blue Pill; the SPI NOR descriptor remains host-mock tested only. The
  [Blue Pill hardware test guide](docs/bluepill_hardware_test.md) records the
  physical results and remaining tests.

See [architecture](docs/architecture.md), [format](docs/record_format.md),
[storage adapters](docs/backend_implementation.md),
[recovery](docs/recovery_and_power_failure.md),
[endurance](docs/flash_endurance.md), and
[integration](docs/integration.md). The original inspected Stage 1 plan remains
in [docs/stage1_architecture_plan.md](docs/stage1_architecture_plan.md).
