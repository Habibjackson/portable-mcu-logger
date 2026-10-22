# STM32F103C8T6 Blue Pill hardware test

This is a separate test target. The intended STM32F103RDT6 production target
keeps its existing linker layout and RAM-backed demonstration application.
The Blue Pill application uses synthetic inputs and a reserved internal-flash
journal; it does not connect to a motor.

## Device and memory layout

| Target | PlatformIO board / CMSIS definition | Startup | SRAM | App flash | Logger flash | Erase page |
| --- | --- | --- | ---: | --- | --- | ---: |
| Blue Pill C8T6 | `bluepill_f103c8` / `STM32F103xB` medium-density family | `startup_stm32f103xb.s` | 20 KiB | `0x08000000..0x0800E000` (56 KiB) | `0x0800E000..0x08010000` (8 KiB) | 1 KiB |
| Production RDT6 | `genericSTM32F103RD` / `STM32F103xE` high-density family | `startup_stm32f103xe.s` | 64 KiB | `0x08000000..0x08058000` (352 KiB) | `0x08058000..0x08060000` (32 KiB) | 2 KiB |

The C8T6 flash partition is 2 KiB critical, 2 KiB event, and 4 KiB routine.
The C8 linker script places vectors at `0x08000000` and cannot place app
sections in the logger region. The post-link ELF verifier requires the exact
target-specific symbols, vector location, and loadable firmware limits. The
flash adapter also rejects reads, writes, and erases outside its linker-bounded
range. It accepts a C8 build's factory flash-size field of 64 or 128 KiB,
but **addresses only the nominal first 64 KiB**; smaller or implausible values
are rejected. The signature is a consistency check, not proof that an
unofficial chip has genuine ST flash or that any extra flash works. The RD
build still requires its expected 384 KiB signature.

These capacities and page sizes match the [ST C8 datasheet](https://www.st.com/resource/en/datasheet/stm32f103c8.pdf),
[ST RD family datasheet](https://www.st.com/resource/en/datasheet/stm32f103rc.pdf),
and [RM0008 flash organization](https://www.st.com/resource/en/reference_manual/rm0008-stm32f103xx-advanced-armbased-32bit-mcus-stmicroelectronics.pdf).
The C8 test layout assumes no custom bootloader or configuration/calibration
data in main flash. Inspect the actual board and any existing flash content
before programming. A flash-resident bootloader or reserved configuration page
requires a reviewed new layout and matching verifier profile; do not use this
image on that board as-is. The STM32 system-memory bootloader is separate from
main flash.

The test firmware selects the internal 8 MHz HSI oscillator and runs its PLL at
64 MHz (`HSI / 2 * 16`), APB1 at 32 MHz, and APB2 at 64 MHz. It does not rely
on the board's external crystal. `USART1` runs at 115200 baud, 8 data bits,
no parity, one stop bit.

## Wiring and programming

Use an SWD programmer such as ST-LINK for flashing and a **3.3 V TTL** USB
serial adapter for the console. Connect common ground, PA13 to SWDIO, PA14 to
SWCLK, and optionally NRST to NRST. Connect Blue Pill PA9 (USART1 TX) to the
adapter RX and PA10 (USART1 RX) to adapter TX. Power the board at the proper
3.3 V or board 5 V input according to its hardware; do not put 5 V logic on
PA9/PA10. PC13 drives the common Blue Pill LED as a latched safe-state
indicator (LED on means safe state); boards wired differently may need a pin
change.

Build, review the reported memory map, then program the test board:

```sh
pio run -e bluepill_f103c8
pio run -e bluepill_f103c8 -t upload
pio device monitor -p COMx -b 115200 -f log2file
```

Replace `COMx` with the USB serial adapter port. The `log2file` filter prints
the actual transcript path. These CLI options are described in the
[PlatformIO run](https://docs.platformio.org/en/latest/core/userguide/cmd_run.html)
and [device monitor](https://docs.platformio.org/en/latest/core/userguide/device/cmd_monitor.html)
documentation. Programming may erase flash; **use the board reset button or
the `reset` command** to test persistence. Do not re-upload between the
before/after readings.

The test firmware polls one received byte per main-loop iteration. Type
commands at a normal human pace; pasting a whole command at once can overrun
the USART receiver. For scripted commands, `tools/bluepill_uart.py` sends
characters 30 ms apart and needs `pyserial` (included with PlatformIO's Python
environment):

```sh
python tools/bluepill_uart.py COMx status
python tools/bluepill_uart.py --output build/bluepill_uart_dump.txt COMx dump
```

Close any other serial monitor before using this helper; Windows allows only
one process to open the COM port at a time.

## Serial commands

The firmware prints the reset cause, factory-reported flash size, its fixed
64 KiB address limit, logger initialization result, and a
read-only count of committed records after every boot. It never formats the
logger region during normal initialization. Type one command per line:

| Command | Effect |
| --- | --- |
| `run`, `stop` | Start or stop one synthetic sensor sample every 100 ms; automatic flash logging starts at a fault. |
| `wave triangle`, `wave saw`, `wave step` | Select motor-speed waveform. |
| `routine` | Queue one telemetry record without a fault transition. |
| `fault current`, `fault pressure`, `fault sensor` | Inject one critical fault. |
| `recover current`, `recover pressure`, `recover sensor` | Record recovery of an active fault. |
| `burst` | Queue 12 routine samples without processing; exercises eight-slot overflow. |
| `status` | Flush pending work, then report samples taken, committed record counts, stored/original bytes and compression ratio, queue and storage drops, storage errors, corrupt scans, page erases, queue occupancy, safe state, and timing maxima. |
| `reset` | Commit a simulated software-reset event, then reset the MCU. |
| `dump` | Print the complete 8 KiB logger region as framed hex. |
| `fill critical` | Fill the two critical pages to exercise `ELOG_STORAGE_FULL`; use this last. Critical pages are never automatically erased. |
| `clear` | Clear the test LED only if there are no active faults or latched application errors. |

The triangle/saw/step speed, gradually rising and falling pressure, and
forward/reverse position deltas are generated in `src/bluepill_main.c`.
Samples 12–16 of each 60-sample cycle inject high current; pressure crosses
23.0 kPa near the middle; samples 42–46 simulate a disconnected sensor. All
three fault types emit recovery records when their condition clears. Fault
handling latches the safe-state LED **before** calling the logger, and logger
errors or dropped routine records also latch it. This is a safety-signal
demonstration, not evidence of motor-control timing or an actuator shutoff.

### Automatic fault-window logging

`run` still checks the synthetic sensors every 100 ms, including while all
readings are healthy. Healthy readings are not queued, so an idle, drained
logger makes no flash program or erase calls. At a threshold crossing the app
queues the critical fault before its first telemetry sample, then saves every
100 ms sensor sample while any fault is detected. It saves one more sample when
the last fault clears and queues a recovery record. This captures the sensor
values that caused the fault and how they changed until recovery. Built-in
pre-fault and post-fault snapshots are disabled for this app; therefore there
is no earlier sensor history in flash. `routine` and `burst` are deliberate
console test commands that still write samples while healthy. `reset` and
manual fault commands also write records by request.

`status` shows `samples=` separately from retained `routine=` records. To
check the policy, start with a drained logger before a synthetic fault,
compare `status` readings during a short healthy interval, and confirm that
`samples` rises while `routine`, `faults`, and `erases_this_boot` stay put.
Continue through a fault and recovery; the record counts should then rise.
Pending fault-window records can still finish committing after the fault
clears. A long fault can rotate the routine pages and overwrite older sensor
samples; queue overflow can drop samples if flash processing falls behind.
Critical fault pages never rotate automatically, so export and service a full
critical partition before relying on further fault persistence.

For `status`, `ratio_pct` is stored payload bytes divided by original payload
bytes across currently retained committed records. It excludes record headers
and flash page overhead. Session counters such as `erases_this_boot` reset on
reboot, while `records` comes from committed flash and survives reset.
`max_process_us` uses the DWT cycle counter around each `elog_process` call;
`max_systick_gap_us` is the largest observed gap between SysTick interrupt
entries. Both assume the configured 64 MHz core clock and are only real
measurements after the firmware runs on hardware.

## Reading records back

Stop sampling, run `status` to drain pending records, then run `dump`. Convert
the captured plain-text serial transcript and decode it:

```sh
python tools/uart_hex_to_bin.py logs/device-monitor-xxxx.log build/bluepill_logs.bin
python tools/decode_logs.py build/bluepill_logs.bin --page-size 1024 --granularity 2 --schema examples/robot_schema.json --human
```

The converter requires a complete `ELOG_HEX_BEGIN 8192` / `ELOG_HEX_END`
block and refuses truncated output. The schema gives the example event IDs,
field names, scaling, and units. An unknown event remains labeled as unmapped
with raw bytes. The decoder also supports JSON output by omitting `--human`.
The logger records the fault classification supplied by the test application;
it cannot independently determine a physical root cause.
The timeline is ordered by committed sequence number. This test app records no
pre-fault telemetry during automatic `run` sampling.

## Test procedure

1. Run the host suite with `cmake -S . -B build -G Ninja`,
   `cmake --build build`, and `ctest --test-dir build --output-on-failure`.
   The core tests check fault priority over pending routine writes, routine and
   fault paths, reboot scans, queue overflow and full critical storage. Codec
   tests compare decompressed bytes to the originals. Fault-injection tests
   interrupt page-header, record-body, commit, and erase operations. The C8
   host test writes through 1 KiB page rotations and checks a patterned
   56 KiB application prefix never changes.
2. On a real board with available critical storage, record the initial
   `status`, start `run`, and compare `status` after a healthy interval before
   sample 12: `samples` should rise without new automatic records. Continue
   through an automatic fault and recovery, then `stop`, `status`, and decode
   a dump. Check that sensor records begin at the fault and include a recovery
   boundary sample. Separately test manual `routine`, `fault current`,
   `recover current`, and `burst`; the latter should report routine queue drops
   and latch the LED.
3. Reset with `reset` after recording the committed count. Observe
   `logger_init=0`, reset cause, and the retained count on the next boot.
   Export and decode a dump to confirm earlier sequences remain. A reset
   during an actual flash program needs controlled lab equipment; the host
   fault-injection suite is the automated interrupted-write check.
4. With a scope or logic analyzer as needed, compare the timing maxima while
   healthy and while `run` is committing fault records. Check actual interrupt and
   control deadlines on the intended hardware. Internal flash operations can
   stall CPU fetches; a passing build cannot establish real-time safety.
5. Use `fill critical` only after exporting desired logs. Verify the full
   condition is reported, the LED remains in safe state, and routine/event
   behavior follows the application policy. Servicing critical storage
   requires an explicit export and erase procedure for this test board.

## Current source verification (2026-10-22)

The CMake host build and all 17 CTest checks pass, including the new
fault-window and compression examples. Both PlatformIO targets build with
their post-link layout checks: `bluepill_f103c8` uses 8,552 of 20,480 RAM
bytes, and `genericSTM32F103RD` uses 10,472 of 65,536 RAM bytes. These are
build and host-test results. The current fault-window firmware has not been
uploaded or measured on the Blue Pill.

## Physical test results (2026-10-18, earlier continuous-logging firmware)

The measurements below describe the firmware used on that date. The
fault-window policy above has passed host tests and built for the Blue Pill,
but has not been programmed or measured on physical hardware.

An ST-LINK V2 was connected to a Blue Pill at about 3.30 V. OpenOCD detected a
Cortex-M3 and medium-density device ID `0x410`. The factory flash-size field
reported **128 KiB**, although the C8 build continues to use only its nominal
first **64 KiB**. The marking and manufacturer were not authenticated. The
board was programmed with `bluepill_f103c8`; OpenOCD reported successful
programming and verification. The logger's 8 KiB region was erased before the
test, and the firmware booted with `logger_ready=1` and `safe_state=0`.

Synthetic logging ran on the board. After a physical RESET-button press, RAM
showed `running=0`, `sample_index=0`, and `logger_ready=1`, while the flash
still held committed records. The strict decoder recovered 91 retained records
with zero integrity warnings: 53 routine telemetry records, 19 faults, and 19
recoveries. The fault timeline included overcurrent, pressure-limit, and
sensor-disconnection transitions and their recoveries. This verifies recovery
of committed records after a board reset; it does not test power loss during a
flash write.

A second, controlled 6.5-second run generated 65 samples and was stopped via
ST-LINK before reading flash. After this run, the strict decoder recovered 98
retained records with zero warnings. Retained payloads totaled 1,744 original
bytes and 1,377 stored bytes (79% of the original payload size; record/page
overhead excluded). A byte-for-byte comparison of the nominal 64 KiB flash
before and after this run found **zero changed bytes outside** the reserved
`0x0800E000..0x08010000` region. The four routine pages changed, and the
logger's session counter reported four page erases. The application image
matched the uploaded binary exactly. Bytes already programmed in the unused
firmware-to-logger gap were present before the controlled run and did not
change.

The critical partition was full during the controlled run
(`storage_full[critical]=1`, one critical storage drop). RAM showed
`safe_state=1` and a stopped sampler after the run. This confirms the test
firmware latched its software safe-state signal on a logging failure; the LED
was not visually checked, and no actuator was connected. The longest measured
`elog_process` call was **22.47 ms** and the largest SysTick entry gap was
**22.41 ms**, calculated from the DWT cycle counter at the configured 64 MHz.
These are board measurements, but no scope trace or motor-control deadline
measurement was taken. Flash logging at this rate is unsuitable for any task
that needs a tighter interrupt response without further scheduling changes.

A CH340 USB-to-UART adapter then appeared as COM4 on this Windows host. Its
`status` output agreed with the earlier ST-LINK measurements, and its 8 KiB
`dump` matched the ST-LINK log dump byte for byte. That image was saved and
strictly decoded before an OpenOCD erase of only the eight reserved 1 KiB
logger pages. A full 64 KiB readback confirmed the region was blank and every
byte outside it was unchanged. After reboot, UART `status` reported zero
records, zero errors, and `safe_state=0`.

On that fresh journal, a standalone `routine` command committed one record.
Standalone `fault current` and `recover current` commands each committed a
record; the safe-state flag latched on the fault and could be cleared after
recovery. A 12-sample `burst` dropped four routine entries from the eight-slot
queue, reported `queue_drops routine=4`, and latched safe state. Separate
pressure-limit and sensor-disconnection fault/recovery commands raised the
committed totals to three faults and three recoveries. The `reset` command
committed a reset record; the captured reboot banner reported a software reset,
`logger_init=0` (success), and 21 retained records with zero scan or storage
errors. The strict decoder found zero warnings, and 14 decoded telemetry and
pre-fault payloads matched the test application's original synthetic values.

`wave saw` and `wave step` were each followed by a bounded routine burst.
Sixteen retained waveform payloads decoded to the expected speeds, including
the step transition to 1,400 rpm. The second boot's queue-drop count reached
eight. The UART `fill critical` command stopped with result `-5`
(`ELOG_STORAGE_FULL`), reported `full critical=1` and one critical storage
drop, and kept `safe_state=1`. The user confirmed that the PC13 LED was on.
A following `routine` command still committed another record. The final UART
image held 74 committed records with zero strict-decoder warnings and matched
the ST-LINK image byte for byte. A before/after 64 KiB comparison found zero
changed bytes outside the reserved logger region.

The host suite passed 9/9 tests, including fault priority over pending
routine records, exact codec round trips, interrupted-write fault injection,
overflow, and flash-reservation checks. Both C8 and production RD targets
built successfully without changing the RD memory layout. The exact
fault-before-pending-routine ordering was tested on the host; it was not
isolated in the UART run. A power interruption *during* a flash write,
production-board deadlines, and a watchdog-triggered reset remain untested on
physical hardware. The ST-LINK could not reattach during a long, continuously
writing run; a physical reset restored the connection. Connect NRST to the
probe if available, and stop logging before extracting records.

The on-board critical partition now contains test records and is full. The
complete final image is saved as `build/bluepill_log_final.bin` in this
workspace. Export it before any further intentional erase.
