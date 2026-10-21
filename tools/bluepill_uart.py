#!/usr/bin/env python3
"""Send one paced command to the Blue Pill USART1 test console."""

import argparse
import sys
import time
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="USB-to-UART port, e.g. COM4 or /dev/ttyUSB0")
    parser.add_argument("command", nargs="+", help="console command words")
    parser.add_argument("--output", type=Path, help="save raw UART response bytes")
    parser.add_argument("--pace-ms", type=float, default=30,
                        help="delay between transmitted characters (default: 30)")
    args = parser.parse_args()
    if not 1 <= args.pace_ms <= 1000:
        parser.error("--pace-ms must be between 1 and 1000")
    command = " ".join(args.command)
    try:
        import serial
    except ImportError:
        parser.error("pyserial is required (available in PlatformIO's Python environment)")
    try:
        with serial.Serial(args.port, 115200, timeout=0.2, write_timeout=1) as link:
            time.sleep(0.2)
            link.reset_input_buffer()
            for byte in (command + "\r").encode("ascii"):
                link.write(bytes([byte]))
                link.flush()
                time.sleep(args.pace_ms / 1000)
            received = bytearray()
            deadline = time.monotonic() + (30 if command == "dump" else 8)
            last_data = time.monotonic()
            while time.monotonic() < deadline:
                chunk = link.read(512)
                if chunk:
                    received.extend(chunk)
                    last_data = time.monotonic()
                    if command == "dump" and b"ELOG_HEX_END" in received:
                        break
                elif received and time.monotonic() - last_data > 1.5:
                    break
    except (OSError, serial.SerialException, UnicodeEncodeError) as exc:
        parser.exit(1, f"UART error: {exc}\n")
    if not received:
        parser.exit(1, "No UART response received\n")
    if command == "dump" and b"ELOG_HEX_END" not in received:
        parser.exit(1, "Incomplete logger dump received\n")
    if args.output:
        args.output.write_bytes(received)
        print(f"saved {len(received)} bytes to {args.output}")
    else:
        sys.stdout.write(received.decode("ascii", errors="replace"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
