# Storage sizing and endurance estimates

The logger rotates EVENT and ROUTINE erase regions in generation order and
counts successful erases in `elog_statistics_t.pages_erased`. This counter is
for the current logger session; it is not a persistent per-page wear meter.
`elog_status_t` reports used and total bytes per priority. CRITICAL does not
auto-erase, so its capacity is a retention limit rather than a circular
endurance pool. Torn tails and headers consume space until eligible for erase.

`tools/estimate_flash_lifetime.py` models one circular partition with fixed
stored record size and uniform wear. Give it the physical size including CRC,
padding, and commit, the erase geometry, actual logging rate, duty cycle,
datasheet cycles under the relevant conditions, and a chosen derating factor:

```sh
python tools/estimate_flash_lifetime.py --rate-hz 10 --record-bytes 122 \
  --page-bytes 2048 --pages 8 --cycles 10000 --derating 0.5
```

For this deliberately heavy STM32 example, a 2 KiB page holds
`floor((2048 - 32) / 122) = 16` records. Ten records per second continuously
give 864,000 records/day, about 54,000 total page erases/day, and roughly
6,750 erases per page per day across eight pages. The idealized 10,000-cycle
point is about **1.48 days**; a 0.5 derating factor reports about **0.74
days**. These are arithmetic projections, not service-life or retention
guarantees. Compression, smaller records, lower sampling rates, and duty cycle
change the result; bad pages, temperature, other writes, and data-retention
requirements can worsen it. The STM32F103RD datasheet's 10,000-cycle figure
applies under its stated conditions. A robot intended to log frequent telemetry
should normally use suitable external storage or a much lower internal-flash
rate.

The host demo provides **synthetic** six-word robot-like telemetry (motor
speed/current, pressure, encoder, state, warning bits). Its DELTA records can
be compared against `NONE` using their stored lengths in the decoded image;
the example is a format demonstration, not a measured workload or CPU timing
benchmark. Keep `NONE` as the conservative default until representative field
data, actual retention requirements, and worst-case CPU time are measured on
the chosen board. A custom codec must also have a matching host decoder and
bounded worst-case time and RAM use.
