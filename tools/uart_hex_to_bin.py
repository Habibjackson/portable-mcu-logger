#!/usr/bin/env python3
"""Extract one ELOG_HEX_BEGIN/END image from a captured UART transcript."""

import argparse
import string
from pathlib import Path


def extract(text):
    lines = text.splitlines()
    for index, line in enumerate(lines):
        marker = line.strip().split()
        if len(marker) != 2 or marker[0] != "ELOG_HEX_BEGIN":
            continue
        length = int(marker[1])
        if length <= 0 or length % 16:
            raise ValueError("dump length must be a positive multiple of 16")
        image = bytearray()
        for body in lines[index + 1:]:
            body = body.strip()
            if body == "ELOG_HEX_END":
                if len(image) != length:
                    raise ValueError(f"incomplete dump: {len(image)} of {length} bytes")
                return bytes(image)
            if len(body) != 32 or any(c not in string.hexdigits for c in body):
                raise ValueError(f"invalid dump line after {len(image)} bytes")
            image.extend(bytes.fromhex(body))
            if len(image) > length:
                raise ValueError("dump exceeds announced length")
        raise ValueError("dump has no ELOG_HEX_END marker")
    raise ValueError("no ELOG_HEX_BEGIN marker found")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("transcript", type=Path, help="captured serial-console text")
    parser.add_argument("output", type=Path, help="raw binary logger image")
    args = parser.parse_args()
    try:
        image = extract(args.transcript.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as exc:
        parser.error(str(exc))
    args.output.write_bytes(image)
    print(f"wrote {len(image)} bytes to {args.output}")


if __name__ == "__main__":
    main()
