# Stage 1: architecture, sizing, and implementation plan

Status: **historical Stage 1 design snapshot**, 18 October 2026. The library,
project linker reservation, host tests, and target compile example have since
been implemented. See the repository README and current docs for actual behavior
and measured memory use. Statements below describing absent files or future
work refer to the initial inspection, not the current project state.

## Repository and toolchain inspection

The supplied workspace contained zero files: no firmware source, linker script, map file, build system, tests, Git repository, `AGENTS.md`, or storage-device description. The available host tools are GCC 15.2.0 (MinGW-w64), CMake 4.1.0, Ninja 1.13.1, and Python 3.12.7. `arm-none-eabi-gcc` was not found. Thus there is no existing firmware memory layout to inspect and no code to build or test at this stage.

### PlatformIO follow-up, 18 October 2026

A generated `platformio.ini` was added after the initial inspection. It selects `ststm32`, `genericSTM32F103RD`, and `stm32cube`. The installed PlatformIO Core is 6.2.0 with ST STM32 platform 19.4.0, STM32CubeF1 1.8.6, and packaged ARM GCC 7.2.1. The board manifest names `stm32f103rdt6` and specifies 384 KiB flash/64 KiB RAM. The `STM32F103xE` build macro is the STM32CubeF1 header's shared high-density family selection and includes the RD variant.

PlatformIO's packaged `STM32F103RDTX_FLASH.ld` assigns **all 384 KiB** at `0x08000000` to firmware and 64 KiB at `0x20000000` to RAM. This is a generic device layout, not proof of a free logger area. No project-owned linker override, bootloader layout, or firmware map exists yet. The first `pio run -e genericSTM32F103RD` reached the ARM linker and failed with `undefined reference to main` because `src/` is empty; STM32Cube and the ARM toolchain did compile. No firmware image or hardware test resulted. Before a logger region is enabled, Stage 6 must introduce a project-owned linker layout and verify the final firmware map.

The part's published geometry can be checked independently of the absent firmware. ST identifies the STM32F103RD as a **384 KiB flash** part, and the [high-density datasheet, DS5792](https://www.st.com/resource/en/datasheet/stm32f103rd.pdf) specifies **64 KiB SRAM** for the 384 KiB variant, 2 KiB flash erase pages, 16-bit programming taking 40–70 µs, page erase taking 20–40 ms, and 10,000 minimum erase cycles under its stated temperature conditions. The [flash programming manual, PM0075](https://www.st.com/resource/en/programming_manual/pm0075-stm32f10xxx-flash-memory-microcontrollers-stmicroelectronics.pdf) states that flash reads, including code/data fetches, stall during a write or erase, that half-word programming requires an erased destination, and that HSI must be on for flash writes/erases. [RM0008](https://www.st.com/resource/en/reference_manual/rm0008-stm32f101xx-stm32f102xx-stm32f103xx-stm32f105xx-and-stm32f107xx-advanced-arm-based-32-bit-mcus-stmicroelectronics.pdf) identifies high-density devices as using 2 KiB pages. These are device facts, not evidence that any particular address is free in the robot firmware.

## Intended architecture

```text
robot application ──> public logger API ──> priority queues ──> record encoder ──> storage journal
                            │                     │                 │                │
                            │                     └─ telemetry rings │                ├─ RAM backend
                            └─ ISR fault ingress                    └─ codec plug-in  ├─ simulated flash
                                                                                      ├─ STM32F103 HAL adapter
                                                                                      └─ descriptor-driven SPI NOR
host decoder <────────────────────────── versioned, explicit little-endian wire format ─┘
```

The core will be C11 with no HAL, CMSIS, RTOS, bus driver, fixed flash address, or allocation after initialization. The caller supplies context/workspace memory, a storage vtable and device instance, a timestamp callback, and optional short critical-section/mutex hooks. One logger instance owns one configured region. EEPROM or FRAM will require another backend, not application changes.

The proposed public API uses `elog_` names in `include/log.h`: initialize, write by priority/type, capture a fault, record recovery, process pending work, flush, get status/statistics, iterate validated records, and recover. All calls return an explicit result such as `OK`, `QUEUED_WITH_DROP`, `INVALID_CONFIG`, `QUEUE_FULL`, `FULL`, `BUSY`, `TIMEOUT`, `IO`, `CORRUPT`, or `UNSUPPORTED`. Payload bytes are copied into bounded queues before a write call returns. The ordinary API runs in the logger task/thread; iteration and recovery require quiescence. `elog_capture_fault_isr()` is the only default ISR entry point: it copies a bounded fault ID, timestamp, and snapshot into a dedicated ingress queue, performs no compression or I/O, and returns immediately with success or an overflow result. It does not claim durable capture at ISR return.

Concurrency contract: one ordinary API producer/consumer context and one designated ISR producer. The ISR queue uses C11 atomics for publication of fixed-size slots, with lock-free support checked for the target; otherwise the ISR API is disabled until a bounded platform implementation is supplied. Multiple task producers need an application mutex or a platform adapter. Multiple ISR producers are not assumed. A short critical section may protect control flags, but no flash call or long copy executes while interrupts are disabled. The control loop and safety response never wait on the logger.

The storage vtable will report capacity, program granularity, maximum program length, erase region containing an address, busy state, and a bounded sync/wait operation. `read`, `program`, and `erase` use offsets relative to the configured device/region, return explicit errors, and specify whether the adapter is synchronous or cooperative. Initialization checks overflow-safe region arithmetic, containment, alignment, minimum erase-region size, maximum record size, and commit capability. The core queries the region at each rotation boundary; it does not assume uniform geometry. A backend must guarantee that a fresh, aligned commit unit can be programmed **after** the body and CRC, or reject this journal format. Device-specific bounds, timeouts, write protection, initialization, and power-up recovery belong in the adapter. No operation may cross an unapproved erase-region boundary.

The STM32F103 adapter will wrap HAL flash calls only after its linker region is proved safe. The SPI NOR adapter will use a device descriptor for address width, page program limit, erase opcodes/geometries, status/busy protocol, timeouts, and any write-protect behavior; no universal NOR command set is assumed. The RAM backend implements the same calls, and the simulated flash backend enforces erased-state programming, geometry, interruption, and injected errors.

## Priority, retention, and real-time behavior

The initial policy has three physically separate partitions within the approved logging region: CRITICAL, EVENT, and ROUTINE. Routine and event traffic cannot consume critical pages. The scheduler services CRITICAL before EVENT before ROUTINE, and checks the critical queue immediately before starting a new routine storage operation. An operation already started is not preemptible. Each `elog_process()` invocation advances at most one bounded storage step; an erase starts only when a platform `may_erase` or safe-state callback permits it. `elog_flush()` may return `BUSY`/`WOULD_BLOCK` rather than violate that policy.

Default queue overflow rules: CRITICAL and EVENT reject the new entry and count the loss; ROUTINE replaces the oldest queued entry, counts that loss, and reports it to the caller. An ISR critical overflow is explicit and increments a counter. Storage-full and backend failures are reported separately from RAM-queue losses. CRITICAL is append-only until explicit export/acknowledgment and reclamation; it never automatically erases the sole valid copy of a committed fault. When critical storage fills, further persistence fails with `FULL`. EVENT and ROUTINE use documented circular retention, reclaiming only their own oldest page; their older records are intentionally lost on rotation. A fault recovery record stays in CRITICAL so routine rotation cannot remove it. Record iteration exposes the retention boundary and gaps.

An active-fault table suppresses a persistent fault logged every control tick. First occurrence queues one critical record with a caller-selected sensor snapshot, repeats saturate a count, recurrence summaries are rate limited, and recovery emits a critical record with the count and duration. A full fault table is an explicit error. The caller controls the event payload schema; the logger does not impose robot-specific fields.

The routine producer writes recent samples into a bounded pre-fault ring. On first fault, that ring is frozen after a published sample boundary and later drained after the fault record. A separate bounded ring retains optional post-fault samples. A sample in progress at the fault boundary may be omitted; neither ring implies nonvolatile retention before processing. Fault capture itself never waits for a compression block. Pre/post-fault telemetry carries a correlation ID and may be dropped if its own partition/queue fills; counters expose this.

Internal flash is a poor default for active motor control. The datasheet's 70 µs maximum per 16-bit program gives about **4.3 ms of programming time** for a 122-byte worst-case example record (61 half-words), before software, verification, and waiting overhead. A page erase can take **40 ms**. PM0075 says flash fetches stall while writes/erases are in progress; interrupt vector fetches, ISR code, constants, and called routines must all be considered. Moving just the flash routine into RAM does not establish a motor deadline bound. DMA is not a substitute for FPEC half-word programming. Until the complete control path is placed/verified appropriately and measured, internal-flash work must be deferred to a controlled safe state, or routine logging should use suitable external storage. Critical queueing remains bounded but durability is then delayed; the application must size RAM and power-hold-up accordingly.

## Record and recovery design

The proposed version-1 wire format uses explicit little-endian serialization, never raw C struct copies. Its 36-byte header is: magic `u32`, version `u8`, header length `u8`, record type `u8`, priority `u8`, codec `u8`, flags `u8`, reserved `u16`, sequence `u64`, timestamp/uptime `u64`, event/fault ID `u32`, original length `u16`, and stored length `u16`. Then come stored payload, CRC-32 over header and payload, alignment padding, and an aligned commit unit programmed last. The commit-unit pattern and CRC variant will be fixed in the format specification in Stage 2. Unknown versions are reported, never decoded as version 1. Timestamps have an explicit uptime/absolute-time flag.

For each append: verify space and erased bytes, serialize/encode in RAM, program header/payload/CRC into fresh program units, wait and verify, then program the separate commit unit. A committed record still requires a valid header, lengths, type/codec, CRC, and marker. A power cut may leave an uncommitted or corrupt tail; recovery scans all page headers and records, accepts only valid commits, seals a torn tail, and resumes in a different erased location. It never rewrites a partially programmed unit. Recovery itself is read-only; reclamation is scheduled later in an allowed safe window. CRC detects many corruptions but is not an atomicity guarantee.

Each partition uses page headers with partition ID, format version, page generation, CRC, and final commit. EVENT and ROUTINE retain an erased spare page, switch to it with a committed header, then reclaim only the oldest page under their retention policy. A power loss during the switch leaves either the old valid page or the new committed page discoverable. CRITICAL does not reclaim committed pages without explicit acknowledgment. A torn page header costs capacity until safely erased. Sequence and page-generation comparisons use unsigned modulo arithmetic with a half-range rule; exactly half-range apart or inconsistent generations are ambiguous and reported as corruption, not silently ordered. With 64-bit counters and bounded region size, normal wrap is safely ordered and the wrap case is testable.

NONE is the baseline codec. DELTA/VARINT operates only on a declared numeric telemetry schema, uses full snapshots at every page start and at a configured interval, and falls back to a full uncompressed snapshot when a delta is not representable or encoding expands the record. State is rebuilt from a validated snapshot after recovery; no delta depends on a record that can be reclaimed independently. BLOCK compression is optional pending a separate CPU/RAM/license evaluation of a maintained embedded codec. Critical records bypass block buffering. The host decoder will share golden format vectors with C round-trip tests.

## Provisional memory budget

This is a sizing example, **not** a reservation or measured `sizeof` result. It assumes an 80-byte maximum queued payload and encoded record, a 16-byte in-RAM slot header (96-byte slot), a 128-byte maximum physical record for devices with at most 8-byte program granularity, and a 32-byte page header. Configuration validation must reject combinations that exceed the smallest erase region or scratch buffer. Actual struct padding, stack use, and backend state will be measured from compiled code in later stages.

| RAM item | Assumption | Bytes |
| --- | ---: | ---: |
| Critical task queue | 8 × 96 | 768 |
| Event queue | 8 × 96 | 768 |
| Routine queue | 16 × 96 | 1,536 |
| ISR fault queue | 4 × 96 | 384 |
| Pre-fault telemetry ring | 32 × 40 | 1,280 |
| Post-fault telemetry ring | 8 × 40 | 320 |
| Encode and scan scratch | 2 × 256 | 512 |
| Page index | 16 × 16 | 256 |
| Active-fault table | 8 × 16 | 128 |
| Context, counters, backend pointers | allowance | 512 |
| **Provisional static total** |  | **6,464 B (~6.3 KiB)** |

The STM32F103RDT6 has 64 KiB SRAM, but the robot's stack, HAL, DMA buffers, control state, and other code are unknown. A later map-file check must compare actual `.bss`/`.data`/stack usage with the complete firmware budget.

An **illustrative only** 32 KiB logging area would comprise 16 high-density 2 KiB pages: 4 critical (8 KiB), 4 event (8 KiB), and 8 routine (16 KiB). With the proposed 36-byte header, 80-byte payload, 4-byte CRC, and 2-byte commit, a full record is 122 bytes; allowing a 32-byte page header yields 16 such records per page. That is roughly 64 full-size critical records if all four pages fill, 48 event records with one spare page, and 112 routine records with one spare page. Actual capacities can be lower because of variable geometry, larger commit alignment, torn tails, snapshots, and page padding. **No physical address is assigned.**

This example also shows why routine internal-flash logging needs a rate limit. At 10 full-size records/s, an 8-page routine ring cycles each page about every `8 × 16 / 10 = 12.8 s`; 10,000 idealized cycles arrive in about 1.48 days. At one record/minute, the same arithmetic gives about 2.43 years. Both estimates exclude temperature, retention, bad pages, writes to other lanes, uneven wear, and operating duty cycle; they are not lifetime guarantees. The Stage 8 calculator will take actual rates, lengths, geometry, erase-cycle specification, and derating as inputs. Frequent telemetry should use a suitable external device or a much lower internal-flash rate.

## Firmware reservation gate

Before enabling the STM32 adapter, obtain the exact production firmware linker script and map file, bootloader and update layout, configuration/calibration regions, part marking, and target HAL version. Choose page-aligned start/end symbols only after proving that code, vector table, data initializers, bootloader, configuration, and other reserved regions do not overlap. Reduce the firmware `FLASH` length or create a separate linker `MEMORY` region and linker `ASSERT`s; validate the final ELF/map in CI. The adapter must check every read/program/erase against both linker symbols and device capacity, and reject mass erase. Check bootloader/update tools separately because they may erase the region outside the logger. No hard-coded example address should be copied into production firmware.

## Implementation stages and evidence gates

| Stage | Deliverable | Gate after implementation |
| --- | --- | --- |
| 1 | This inspected architecture, budget, and plan | Review sources and arithmetic; no build/test target exists yet. |
| 2 | Public headers, C11 format serializer/parser, CRC, RAM backend, host CMake targets | Build with strict warnings; golden byte-order/length/version tests; report real `sizeof`. |
| 3 | Fixed queues, scheduler, ISR capture, telemetry rings, fault dedup, counters | Priority, overflow, copy-ownership, recurrence, and documented concurrency tests. |
| 4 | Geometry-aware simulated flash, append journal, commit, scan/recover | Inject power cuts at every program/commit/erase stage; CRC, full-region, timeout, torn-tail, and wrap tests. |
| 5 | Delta/varint codec, snapshot rules, host decoder; evaluate optional block codec | Lossless round trips, expansion fallback, broken-chain recovery, representative telemetry benchmark. |
| 6 | STM32F103 internal-flash HAL adapter and robot integration | Linker/ELF overlap check, host adapter mocks, target build, flash latency audit; hardware claims only after hardware tests. |
| 7 | Descriptor-driven SPI NOR backend | Test actual selected chip descriptor's page/sector limits, busy/timeout/error behavior and recovery. |
| 8 | Rotation/wear accounting, lifetime calculator, fault-injection suite, full docs | Wear distribution and retention tests, model inputs checked; sanitizers where host compiler supports them. |
| 9 | Portability, memory, real-time, and fault-path review | Clean host build/tests, actual RAM/flash map, documented residual risks and target evidence. |

Each stage should build, run the tests that then exist, fix failures, and summarize changed files, assumptions, and limits before advancing. A passing host suite does not establish power-failure behavior on the board.

## Inputs still needed for target integration

- Production linker script and map file, bootloader/update policy, and configuration flash layout.
- Motor-control interrupt deadline, safe-state definition, task scheduling model, and power-fail hold-up budget.
- Actual SPI NOR part number and datasheet, if external flash will be used.
- Expected telemetry/event/fault rates, desired retention duration, payload schemas, and maximum sensor snapshot size.
- Chosen STM32CubeF1 HAL version and ARM toolchain for Stage 6.

These inputs affect target reservations, timing guarantees, and production defaults. They do not prevent the portable host stages from proceeding.
