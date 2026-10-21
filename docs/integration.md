# Integration guide

## Host and PlatformIO examples

The [examples guide](../examples/README.md) starts with a short RAM-backed
program, then shows reboot recovery after a simulated torn write and an
ISR fault path that disables outputs before capture. Each has a CMake target
and a runnable host executable.

The host demo in `examples/host_demo/main.c` sets up sixteen 2 KiB RAM erase
regions, configures 4/4/8 pages for critical/event/routine, writes sample
telemetry and fault transitions, and exports the raw image. `src/main.c` is the
PlatformIO STM32Cube compile example; it uses a small RAM image and placeholder
sensor values, so it is safe to build while the real board's flash layout and
motor timing are unknown. Its RAM image is formatted on every boot because it
is only a demo; do not format nonvolatile storage during normal logger init.

For a raw dump containing only the illustrative 32 KiB logger region:

```sh
python tools/decode_logs.py logger_region.bin --page-size 2048 --granularity 2
```

The decoder emits JSON with sequence, type, priority, lengths, decoded payload
hex, and signed numeric telemetry when applicable. Add
`--schema examples/robot_schema.json --human` for named events, engineering
units, fault durations, and a readable timeline. `--strict` exits nonzero on
corruption warnings; `--regions file.json` supplies a list of `{start,length}`
entries for variable-size erase regions in a dump. Maintain a versioned
application schema alongside any firmware changes to event IDs or payload
layouts. Custom codec IDs
need an application decoder; the generic tool reports their decode error.

The STM32F103C8T6 Blue Pill test build has a separate 1 KiB-page flash
reservation and a UART dump command. Its wiring, programming, and recovery
procedure are in [bluepill_hardware_test.md](bluepill_hardware_test.md).

## Switching storage without changing logging calls

Construct `elog_storage_t storage` from one of:

| Backend | Initialization | Additional input |
| --- | --- | --- |
| RAM | `elog_ram_storage_init` | Byte array, erase-region table, granularity |
| STM32F103 | `elog_stm32f103_flash_init` | Reviewed linker symbols and safe-state policy |
| SPI NOR | `elog_spi_nor_init` | Actual chip descriptor and bounded SPI bus callbacks |

Set `config.partition[0..2]` to non-overlapping device-relative ranges that
consist of whole erase regions. For the **example** linker reservation, the
internal-flash offsets are `0x58000..0x60000`: 8 KiB critical at
`0x58000..0x5A000`, 8 KiB event at `0x5A000..0x5C000`, and 16 KiB routine at
`0x5C000..0x60000`. These are illustrative only. For the HAL adapter,
`storage_timeout_ms` must be at least its packaged
`FLASH_TIMEOUT_VALUE` (50,000 ms). All three partitions must be changed when
the approved reservation changes. The same calls to `elog_write`,
`elog_fault`, `elog_event`, `elog_process`, and `elog_iterate_records` work
with each backend.

When using internal flash, only call `elog_process` from a control-safe state,
and set `config.may_erase` to a callback that allows erases only in that state.
The callback alone does not gate program operations. The ISR API copies a
fault into RAM immediately but cannot make it durable until processing runs.
Size queue and power hold-up for that delay. Serializing ordinary API calls is
the application's responsibility; there is no built-in RTOS mutex. Supply a
monotonic 64-bit millisecond timestamp if intervals or fault durations are
used. If using HAL's 32-bit tick, extend it across wrap in the task context.

## Production checks before hardware use

- Confirm the board is an STM32F103RDT6 (or adapt the HAL backend, geometry,
  linker script, and verifier for the actual part).
- Inspect the production map and all bootloader/update/configuration erase
  ranges. The example post-link verifier covers only the linked image.
- Measure worst-case flash operation latency and every motor-control ISR's
  deadline on hardware. If flash stalls violate safety timing, use a safe
  maintenance window or suitable external storage.
- Select and validate the actual SPI NOR datasheet descriptor, address mode,
  protection state, power-up state, bus timeout behavior, and JEDEC identity.
- Define an export and service procedure for full CRITICAL pages; the logger
  intentionally does not erase them automatically.
- Exercise controlled power cuts, reset during program/erase, CRC failures,
  and long-duration wrap behavior on the real device before durability claims.
