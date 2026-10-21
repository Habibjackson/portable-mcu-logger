# Storage backends and flash protection

## Shared contract

The `elog_storage_ops_t` methods in `include/log_storage.h` use device-relative
byte offsets. A `program` call writes aligned units that are entirely erased;
an `erase` call erases exactly one region reported by `erase_region`. Calls
are synchronous: they return success only after completion and verification.
`is_busy` returns a result and a busy flag so bus/status errors are reported.
The caller owns transfer buffers until the call returns. A backend may split a
program request at a chip program-page boundary. The core validates every
configured partition against the backend geometry and keeps all operations
inside its partitions; adapters additionally enforce their own allowed range.

The RAM backend models erased-state programming and accepts a table of
contiguous, potentially different erase-region sizes. `elog_ram_storage_format`
is an explicit test/demo operation. Logger initialization never calls it. The
simulated flash wrapper can cut selected program and erase calls after a byte
prefix and report a timeout or I/O error. It approximates digital power cuts;
it does not model analog flash behavior or guarantee hardware power-loss safety.

## STM32F103RDT6 internal flash

The [STM32F103RD datasheet](https://www.st.com/resource/en/datasheet/stm32f103rd.pdf)
identifies 384 KiB internal flash, 64 KiB SRAM, 2 KiB high-density erase pages,
16-bit program units, 10,000 minimum erase cycles under its stated conditions,
40-70 us half-word program time, and 20-40 ms page erase time. The
[flash programming manual](https://www.st.com/resource/en/programming_manual/pm0075-stm32f10xxx-flash-memory-microcontrollers-stmicroelectronics.pdf)
requires an erased programming destination and HSI during flash writes/erases.
Flash fetches stall during these operations. These published times are device
specifications, not measured application latency.

`ldscripts/STM32F103RDTX_LOGGER.ld` assigns firmware 352 KiB at
`0x08000000..0x08058000` and an illustrative logger area of 32 KiB at
`0x08058000..0x08060000`, exposing `__elog_start` and `__elog_end`. The adapter
converts those linker symbols to offsets and checks every read, program, and
page erase against both that range and the 384 KiB physical limit. It never
offers a mass-erase operation. The script has alignment and overlap assertions,
and `tools/platformio_verify.py` runs `tools/verify_flash_layout.py` after the
PlatformIO link to inspect loadable ELF segments and allocated flash sections.
This proves the example firmware image does not overlap the example logger
range. It cannot inspect a separate bootloader, external updater, flash
configuration store, or an image uploaded by another tool.

Before enabling internal-flash logging on the robot, verify the exact part
marking, production linker/map file, vector table, bootloader and update erase
policy, configuration/calibration pages, and power-fail behavior. Adjust the
linker symbols and three partitions together. Use `elog_stm32f103_flash_init`
only after that review. The adapter uses HAL page erase and half-word program,
checks HSI readiness, verifies each programmed half-word and erased page, and
rejects a requested timeout shorter than HAL's `FLASH_TIMEOUT_VALUE` (50 s in
the packaged HAL). HAL's timeout uses the HAL tick; a stalled flash fetch can
affect that tick, so this is **not** a hard real-time deadline guarantee.

At 70 us per half-word, a 122-byte example record needs about 4.3 ms of flash
program time before software and verification overhead; a page erase can take
40 ms. The interrupt vector fetch, ISR code, constants, and called routines
may all depend on the same flash. Moving only the flash function into RAM does
not remove those dependencies. Run internal-flash processing only in a
validated safe state, or use suitable external storage when control deadlines
cannot tolerate the stall. `may_erase` guards erase starts only; the
application must also gate `elog_process` itself for internal-flash program
operations. No hardware timing or motor deadline test has been performed here.

## STM32F103C8T6 Blue Pill test target

The separate `bluepill_f103c8` build uses the medium-density `STM32F103xB`
device definition, 20 KiB SRAM, nominal 64 KiB flash, and 1 KiB erase pages.
Its dedicated linker script reserves `0x0800E000..0x08010000` for the logger
and leaves the first 56 KiB for the test application and vectors. The same
HAL adapter selects 64 KiB/1 KiB geometry at compile time, accepts a factory
size report of either 64 or 128 KiB while retaining the 64 KiB limit, and
applies the linker bounds to each
read, program, and erase. The linker and ELF verifier also require the C8
vector and log positions. The factory signature cannot establish that a
clone is genuine; no code uses flash beyond the C8's nominal 64 KiB.

The test application and its wiring, console commands, and recovery steps are
documented in [bluepill_hardware_test.md](bluepill_hardware_test.md). It has no
motor or real-time control loop. Its timing counters need physical measurement
before drawing any interrupt-latency conclusion.

## External SPI NOR

`elog_spi_nor_init` takes a device descriptor with capacity, allowed bounds,
address width, program-page size/granularity, opcodes, WIP/WEL status bits,
poll limit, and a table of variable erase ranges/opcodes. It validates range
coverage and boundaries, splits writes at program pages, checks for erased
bytes, polls completion, and reads back after program/erase. The bus callbacks
own chip select, full-duplex transfer, and a wrapping 32-bit millisecond clock.
They must themselves be bounded and correctly serialize use of the SPI bus.

This adapter handles ordinary 1-1-1 command/address/data transactions. It
does not discover a chip or enter its 4-byte address mode, negotiate dummy
cycles, unlock protection bits, or validate a particular JEDEC ID. Choose the
descriptor from the actual NOR datasheet and test it on that device before
using it for robot faults. A host mock covers geometry, cross-page writes,
erase commands, and status error propagation; no physical NOR has been tested.
