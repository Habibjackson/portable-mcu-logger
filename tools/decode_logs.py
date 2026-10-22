#!/usr/bin/env python3
"""Decode version-1 ELOG images from a device-relative raw flash dump."""

import argparse
import json
import struct
import sys
import zlib
from decimal import Decimal, InvalidOperation
from pathlib import Path

MAGIC_PAGE = b"ELPG"
MAGIC_RECORD = b"ELOG"
MASK64 = (1 << 64) - 1
PRIORITIES = ("critical", "event", "routine")
RECORD_TYPES = {1: "telemetry", 2: "event", 3: "fault",
                4: "recovery", 5: "fault repeat", 6: "reset",
                7: "pre-fault telemetry", 8: "post-fault telemetry"}
FIELD_TYPES = {"u8": "B", "u16": "H", "u32": "I", "i32": "i"}


def newer(a, b):
    """Compare wrapping 64-bit sequence numbers; reject a half-range tie."""
    difference = (a - b) & MASK64
    if difference == 1 << 63:
        raise ValueError("ambiguous sequence/generation wrap")
    return difference != 0 and difference < 1 << 63


def crc32(data):
    """Return the format's reflected CRC-32/IEEE as an unsigned 32-bit value."""
    return zlib.crc32(data) & 0xFFFFFFFF


def regions_from_args(size, page_size, regions_path):
    """Build a gap-free erase-region map for the complete raw image.

    A JSON map supports variable erase sizes; otherwise page_size is repeated.
    Reject partial coverage before any record parsing begins.
    """
    if regions_path:
        regions = json.loads(Path(regions_path).read_text(encoding="utf-8"))
        result = [(int(x["start"]), int(x["length"])) for x in regions]
    else:
        if page_size <= 0 or size % page_size:
            raise ValueError("dump size must be a multiple of --page-size")
        result = [(start, page_size) for start in range(0, size, page_size)]
    if not result or result[0][0] != 0:
        raise ValueError("regions must start at offset zero")
    position = 0
    for start, length in result:
        if start != position or length < 160 or start + length > size:
            raise ValueError("regions must cover the dump without gaps or overlap")
        position += length
    if position != size:
        raise ValueError("regions do not cover the whole dump")
    return result


def read_varint(data, position):
    """Return one unsigned LEB128 value and the next byte offset.

    Truncated, unterminated, and overflowing values raise ValueError.
    """
    value = 0
    for index in range(10):
        if position >= len(data):
            raise ValueError("truncated varint")
        byte = data[position]
        position += 1
        if index == 9 and byte & 0xFE:
            raise ValueError("varint overflow")
        value |= (byte & 0x7F) << (7 * index)
        if not byte & 0x80:
            return value, position
    raise ValueError("unterminated varint")


def unzigzag(value):
    """Reverse the signed-integer ZigZag mapping used by numeric codecs."""
    return (value >> 1) if value & 1 == 0 else -((value >> 1) + 1)


def decode_numeric(data, codec, original_length, decoded_by_seq):
    """Return uncompressed payload bytes for NONE, VARINT, or DELTA.

    DELTA requires its referenced base sequence in decoded_by_seq. Unknown
    codecs, broken chains, invalid lengths, and trailing bytes raise ValueError.
    """
    if codec == 0:
        if len(data) != original_length:
            raise ValueError("NONE length mismatch")
        return data
    if codec == 2:
        if not data:
            raise ValueError("empty VARINT stream")
        count, position = data[0], 1
        previous = None
    elif codec == 1:
        if len(data) < 9:
            raise ValueError("short DELTA stream")
        base = int.from_bytes(data[:8], "little")
        previous = decoded_by_seq.get(base)
        if previous is None:
            raise ValueError(f"missing DELTA base sequence {base}")
        count, position = data[8], 9
    else:
        raise ValueError(f"unsupported codec {codec}")
    if count == 0 or count > 20 or original_length != 4 * count:
        raise ValueError("invalid numeric word count")
    if previous is not None and len(previous) != original_length:
        raise ValueError("DELTA base length mismatch")
    output = bytearray()
    for index in range(count):
        encoded, position = read_varint(data, position)
        if codec == 2 and encoded > 0xFFFFFFFF:
            raise ValueError("VARINT value overflow")
        if codec == 1 and encoded > 0x1FFFFFFFF:
            raise ValueError("DELTA value overflow")
        value = unzigzag(encoded)
        if previous is not None:
            value += struct.unpack_from("<i", previous, 4 * index)[0]
        if not -(1 << 31) <= value < 1 << 31:
            raise ValueError("decoded numeric value overflow")
        output.extend(struct.pack("<i", value))
    if position != len(data):
        raise ValueError("trailing codec bytes")
    return bytes(output)


def parse_image(image, regions, granularity):
    """Validate committed pages and records, then decode in sequence order.

    Return (records, warnings). Torn tails and codec failures become warnings;
    only validated commits are returned as records. The caller may use --strict
    to make any warning a command-line failure.
    """
    if granularity < 1 or granularity > 8 or 32 % granularity:
        raise ValueError("program granularity must divide the 32-byte page header")
    records = []
    warnings = []
    for start, length in regions:
        header = image[start:start + 32]
        if header == b"\xff" * 32:
            if image[start + 32:start + length] != b"\xff" * (length - 32):
                warnings.append(f"page {start}: erased header with programmed body")
            continue
        if header[:4] != MAGIC_PAGE:
            warnings.append(f"page {start}: torn or unknown header")
            continue
        if header[4] != 1:
            warnings.append(f"page {start}: unsupported version {header[4]}")
            continue
        lane = header[5]
        if (lane > 2 or header[6:8] != b"\x20\x00" or
                int.from_bytes(header[16:20], "little") != crc32(header[:16]) or
                header[20:32 - granularity] != b"\xff" * (12 - granularity) or
                header[32 - granularity:32] != b"\xa5" * granularity):
            warnings.append(f"page {start}: invalid page CRC/commit")
            continue
        generation = int.from_bytes(header[8:16], "little")
        position = start + 32
        end = start + length
        while position + 36 <= end:
            head = image[position:position + 36]
            if head == b"\xff" * 36:
                if image[position:end] != b"\xff" * (end - position):
                    warnings.append(f"page {start}, offset {position}: programmed bytes after erased gap")
                break
            if head[:4] != MAGIC_RECORD or head[4] != 1 or head[5] != 36:
                warnings.append(f"page {start}, offset {position}: torn record header")
                break
            record_type, priority, codec, flags = head[6:10]
            original_length, stored_length = struct.unpack_from("<HH", head, 32)
            body_size = 36 + stored_length + 4
            commit_offset = position + ((body_size + granularity - 1) // granularity) * granularity
            record_end = commit_offset + granularity
            if (record_end > end or record_end - position > 128 or priority != lane or
                    record_type not in range(1, 9) or flags & ~1 or head[10:12] != b"\0\0"):
                warnings.append(f"page {start}, offset {position}: invalid record length/header")
                break
            payload = image[position + 36:position + 36 + stored_length]
            crc_at = position + 36 + stored_length
            recorded_crc = int.from_bytes(image[crc_at:crc_at + 4], "little")
            if (recorded_crc != crc32(image[position:crc_at]) or
                    image[crc_at + 4:commit_offset] != b"\xff" * (commit_offset - crc_at - 4) or
                    image[commit_offset:record_end] != b"\xa5" * granularity):
                warnings.append(f"page {start}, offset {position}: invalid CRC/commit")
                break
            sequence, timestamp, event_id = struct.unpack_from("<QQI", head, 12)
            records.append({"sequence": sequence, "timestamp": timestamp,
                            "event_id": event_id, "type": record_type,
                            "priority": priority, "codec": codec,
                            "flags": flags, "original_length": original_length,
                            "stored_length": stored_length, "page_generation": generation,
                            "offset": position, "_payload": payload})
            position = record_end
    if records:
        newest = records[0]["sequence"]
        for record in records[1:]:
            if newer(record["sequence"], newest):
                newest = record["sequence"]
        records.sort(key=lambda record: -((newest - record["sequence"]) & MASK64))
    decoded_by_seq = {}
    for record in records:
        try:
            decoded = decode_numeric(record.pop("_payload"), record["codec"],
                                     record["original_length"], decoded_by_seq)
            record["payload_hex"] = decoded.hex()
            if record["type"] == 1 and len(decoded) % 4 == 0:
                record["numeric_i32"] = list(struct.unpack("<" + "i" * (len(decoded) // 4), decoded))
            decoded_by_seq[record["sequence"]] = decoded
        except ValueError as exc:
            warnings.append(f"record {record['sequence']}: {exc}")
            record["decode_error"] = str(exc)
    return records, warnings


def load_schema(path):
    """Load application event names and payload layouts, separate from ELOG."""
    schema = json.loads(Path(path).read_text(encoding="utf-8"))
    if (not isinstance(schema, dict) or schema.get("schema_version") != 1 or
            not isinstance(schema.get("events"), dict)):
        raise ValueError("schema needs schema_version 1 and an events object")
    for event_id, definition in schema["events"].items():
        if (not event_id.isdecimal() or not isinstance(definition, dict) or
                not isinstance(definition.get("name"), str)):
            raise ValueError(f"invalid event schema entry {event_id!r}")
        if "fields" in definition and not isinstance(definition["fields"], list):
            raise ValueError(f"event {event_id}: fields must be a list")
    return schema


def decode_fields(payload, fields):
    """Interpret a fixed little-endian payload without losing the raw values."""
    values = {}
    parts = []
    position = 0
    for field in fields:
        name = field["name"]
        kind = field["type"]
        if kind not in FIELD_TYPES:
            raise ValueError(f"field {name}: unsupported type {kind}")
        size = struct.calcsize("<" + FIELD_TYPES[kind])
        if position + size > len(payload):
            raise ValueError(f"field {name}: payload is too short")
        raw = struct.unpack_from("<" + FIELD_TYPES[kind], payload, position)[0]
        position += size
        if "enum" in field:
            value = field["enum"].get(str(raw), f"unknown({raw})")
        elif "bits" in field:
            bit_names = field["bits"]
            known_mask = sum(1 << int(bit) for bit in bit_names)
            labels = [label for bit, label in bit_names.items()
                      if raw & (1 << int(bit))]
            unknown = (raw & 0xFFFFFFFF) & ~known_mask
            if unknown:
                labels.append(f"unknown:0x{unknown:X}")
            value = "|".join(labels) if labels else "none"
        elif "scale" in field:
            try:
                value = format(Decimal(raw) * Decimal(field["scale"]), "f")
            except (InvalidOperation, TypeError) as exc:
                raise ValueError(f"field {name}: invalid scale") from exc
            if "." in value:
                value = value.rstrip("0").rstrip(".")
        else:
            value = raw
        unit = field.get("unit", "")
        values[name] = {"raw": raw, "value": value, "unit": unit}
        parts.append(f"{name}={value}{' ' + unit if unit else ''}")
    if position != len(payload):
        raise ValueError(f"payload has {len(payload) - position} extra bytes")
    return values, parts


def annotate_records(records, schema):
    """Attach human meaning; preserve undecodable bytes and report mismatches."""
    warnings = []
    for record in records:
        definition = schema["events"].get(str(record["event_id"]))
        name = definition["name"] if definition else f"unmapped:0x{record['event_id']:X}"
        meaning = {"name": name, "fields": {}}
        record["meaning"] = meaning
        if "decode_error" in record:
            meaning["summary"] = f"codec error: {record['decode_error']}"
            continue
        payload = bytes.fromhex(record["payload_hex"])
        parts = []
        try:
            if record["type"] == 4:
                if len(payload) < 12:
                    raise ValueError("recovery prefix is shorter than 12 bytes")
                repeats, duration = struct.unpack_from("<IQ", payload)
                meaning["repeat_count"] = repeats
                meaning["duration_ms"] = duration
                parts += [f"repeats={repeats}", f"duration={duration} ms"]
                payload = payload[12:]
            elif record["type"] == 5:
                if len(payload) != 4:
                    raise ValueError("fault repeat needs a 4-byte count")
                repeats = struct.unpack("<I", payload)[0]
                meaning["repeat_count"] = repeats
                parts.append(f"repeats={repeats}")
                payload = b""
            if definition and record["type"] != 5:
                fields = (definition.get("recovery_fields")
                          if record["type"] == 4 else definition.get("fields"))
                if fields is not None and payload:
                    meaning["fields"], decoded_parts = decode_fields(payload, fields)
                    parts += decoded_parts
                elif fields is not None and record["type"] != 4 and not payload:
                    if definition.get("optional_payload"):
                        parts.append("no snapshot")
                    elif fields:
                        raise ValueError("required payload is empty")
                elif definition.get("encoding") == "utf8" and payload:
                    message = payload.decode("utf-8")
                    meaning["message"] = message
                    parts.append(f"message={message!r}")
            if payload and not parts:
                parts.append(f"raw=0x{payload.hex()}")
            elif payload and record["type"] == 4 and not (
                    definition and definition.get("recovery_fields")):
                parts.append(f"extra=0x{payload.hex()}")
            if not parts:
                parts.append("no payload")
        except (ValueError, UnicodeDecodeError, KeyError, TypeError) as exc:
            warnings.append(f"record {record['sequence']}: schema decode: {exc}")
            parts = [f"schema mismatch; raw=0x{record['payload_hex']}"]
            meaning["fields"] = {}
        meaning["summary"] = ", ".join(parts)
    return warnings


def render_human(records, warnings, schema):
    """Render one validated, sequence-ordered record per line."""
    lines = []
    time_unit = schema.get("timestamp_unit", "uptime units") if schema else "uptime units"
    for record in records:
        priority = PRIORITIES[record["priority"]]
        kind = RECORD_TYPES[record["type"]]
        clock = "absolute" if record["flags"] & 1 else time_unit
        meaning = record.get("meaning", {})
        name = meaning.get("name", f"id=0x{record['event_id']:X}")
        detail = meaning.get("summary")
        if detail is None:
            detail = (f"numeric_i32={record['numeric_i32']}" if
                      "numeric_i32" in record else
                      f"raw=0x{record.get('payload_hex', '')}")
        lines.append(f"#{record['sequence']} {record['timestamp']} {clock} | "
                     f"{priority} {kind} {name} | {detail}")
    lines.extend(f"WARNING: {warning}" for warning in warnings)
    return "\n".join(lines)


def main():
    """Parse CLI options and print JSON or a schema-aware human timeline."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="raw device-relative flash dump")
    parser.add_argument("--page-size", type=int, default=2048)
    parser.add_argument("--granularity", type=int, default=2)
    parser.add_argument("--regions", type=Path, help="JSON list of {start,length} erase regions")
    parser.add_argument("--schema", type=Path, help="application event and payload schema JSON")
    parser.add_argument("--human", action="store_true", help="print a readable timeline")
    parser.add_argument("--strict", action="store_true", help="exit with error on any corruption warning")
    args = parser.parse_args()
    try:
        image = args.image.read_bytes()
        regions = regions_from_args(len(image), args.page_size, args.regions)
        records, warnings = parse_image(image, regions, args.granularity)
        schema = load_schema(args.schema) if args.schema else None
        if schema:
            warnings.extend(annotate_records(records, schema))
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        parser.error(str(exc))
    if args.human:
        print(render_human(records, warnings, schema))
    else:
        print(json.dumps({"records": records, "warnings": warnings}, indent=2))
    return 2 if args.strict and warnings else 0


if __name__ == "__main__":
    sys.exit(main())
