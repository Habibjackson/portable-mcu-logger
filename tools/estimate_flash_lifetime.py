#!/usr/bin/env python3
"""Estimate one circular log partition's flash use and idealized wear."""

import argparse
import json
import math


def estimate(rate_hz, record_bytes, page_bytes, page_count, cycles,
             derating, duty_cycle, header_bytes):
    """Estimate idealized circular-partition wear from rate and geometry.

    Return per-day write and erase counts plus derated lifetime. The model
    assumes uniform rotation and a fixed physical record size; it is not a
    device endurance or retention guarantee.
    """
    if not (rate_hz >= 0 and record_bytes > 0 and page_bytes > header_bytes > 0
            and page_count >= 2 and cycles > 0 and 0 < derating <= 1
            and 0 < duty_cycle <= 1):
        raise ValueError("invalid rate, geometry, endurance, or derating")
    records_per_page = (page_bytes - header_bytes) // record_bytes
    if records_per_page == 0:
        raise ValueError("record does not fit on one erase page")
    average_rate = rate_hz * duty_cycle
    records_per_day = average_rate * 86400
    bytes_per_day = records_per_day * record_bytes
    pages_per_day = records_per_day / records_per_page
    erases_per_page_per_day = pages_per_day / page_count
    ideal_days = math.inf if erases_per_page_per_day == 0 else cycles / erases_per_page_per_day
    return {
        "records_per_day": records_per_day,
        "bytes_per_day": bytes_per_day,
        "records_per_erase_page": records_per_page,
        "approx_total_page_erases_per_day": pages_per_day,
        "approx_erases_per_page_per_day": erases_per_page_per_day,
        "idealized_lifetime_days": ideal_days,
        "derated_lifetime_days": ideal_days * derating,
        "usable_pages_with_one_spare": page_count - 1,
        "assumptions": {
            "rate_hz_when_active": rate_hz,
            "duty_cycle": duty_cycle,
            "record_bytes": record_bytes,
            "erase_page_bytes": page_bytes,
            "page_count": page_count,
            "header_bytes": header_bytes,
            "specified_erase_cycles": cycles,
            "derating_factor": derating,
        },
        "notice": "Approximation, not a service-life or retention guarantee. "
                  "Assumes uniform wear and fixed stored record size; excludes "
                  "torn writes, bad pages, temperature, data retention, and downtime."
    }


def main():
    """Parse workload inputs and print the estimate and assumptions as JSON."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rate-hz", type=float, required=True,
                        help="records per second while active")
    parser.add_argument("--record-bytes", type=int, required=True,
                        help="physical bytes per record, including commit/padding")
    parser.add_argument("--page-bytes", type=int, required=True)
    parser.add_argument("--pages", type=int, required=True)
    parser.add_argument("--cycles", type=int, required=True,
                        help="datasheet erase endurance at applicable conditions")
    parser.add_argument("--derating", type=float, default=1.0)
    parser.add_argument("--duty-cycle", type=float, default=1.0)
    parser.add_argument("--header-bytes", type=int, default=32)
    args = parser.parse_args()
    try:
        result = estimate(args.rate_hz, args.record_bytes, args.page_bytes,
                          args.pages, args.cycles, args.derating,
                          args.duty_cycle, args.header_bytes)
    except ValueError as exc:
        parser.error(str(exc))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
