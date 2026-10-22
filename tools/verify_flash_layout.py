#!/usr/bin/env python3
"""Validate STM32F103C8/RD ELF firmware and reserved logger flash ranges."""

import argparse
import struct
from pathlib import Path


FLASH_BASE = 0x08000000
FLASH_ADDRESS_SPACE_END = 0x09000000
TARGETS = {
    "rd": {"flash_bytes": 384 * 1024, "page_bytes": 2048,
           "log_start": 0x08058000, "log_bytes": 32 * 1024},
    "c8": {"flash_bytes": 64 * 1024, "page_bytes": 1024,
           "log_start": 0x0800E000, "log_bytes": 8 * 1024},
}


def verify(path, target="rd"):
    """Check one ELF against the selected MCU's reserved flash layout.

    Validate vectors, linker symbols, and loadable/allocated firmware ranges.
    Return the loaded end and logger bounds; raise ValueError on any mismatch.
    The check cannot prove what unrelated data is already on a physical board.
    """
    if target not in TARGETS:
        raise ValueError(f"unsupported target {target}")
    profile = TARGETS[target]
    flash_end = FLASH_BASE + profile["flash_bytes"]
    expected_log_start = profile["log_start"]
    data = Path(path).read_bytes()
    if len(data) < 52 or data[:4] != b"\x7fELF" or data[4:6] != b"\x01\x01":
        raise ValueError("expected a little-endian ELF32 file")
    header = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    machine, entry = header[1], header[3]
    phoff, shoff = header[4], header[5]
    phentsize, phnum = header[8], header[9]
    shentsize, shnum, shstrndx = header[10], header[11], header[12]
    if machine != 40 or phentsize < 32 or shentsize < 40 or shstrndx >= shnum:
        raise ValueError("expected a 32-bit Arm ELF with standard headers")
    if phoff + phentsize * phnum > len(data) or shoff + shentsize * shnum > len(data):
        raise ValueError("ELF header table exceeds file size")
    sections = [struct.unpack_from("<IIIIIIIIII", data, shoff + shentsize * i)
                for i in range(shnum)]
    strings_section = sections[shstrndx]
    if strings_section[4] + strings_section[5] > len(data):
        raise ValueError("section-name table exceeds file size")
    section_names = data[strings_section[4]:strings_section[4] + strings_section[5]]
    vector_sections = []
    for section in sections:
        name_offset = section[0]
        if name_offset >= len(section_names):
            continue
        name_end = section_names.find(b"\0", name_offset)
        if name_end >= 0 and section_names[name_offset:name_end] == b".isr_vector":
            vector_sections.append(section)
    if (len(vector_sections) != 1 or vector_sections[0][3] != FLASH_BASE or
            vector_sections[0][5] < 8):
        raise ValueError("interrupt vectors must start at application flash base")
    if not FLASH_BASE <= entry < expected_log_start:
        raise ValueError("reset entry lies outside application flash")
    symbols = {}
    for section in sections:
        if section[1] != 2 or section[9] < 16:
            continue
        offset, size, link, entry_size = section[4], section[5], section[6], section[9]
        if link >= shnum or offset + size > len(data):
            raise ValueError("invalid ELF symbol table")
        strings = sections[link]
        string_data = data[strings[4]:strings[4] + strings[5]]
        for position in range(offset, offset + size, entry_size):
            name_offset, value = struct.unpack_from("<II", data, position)
            if name_offset >= len(string_data):
                continue
            end = string_data.find(b"\0", name_offset)
            if end < 0:
                continue
            name = string_data[name_offset:end].decode("ascii", errors="ignore")
            symbols[name] = value
    if "__elog_start" not in symbols or "__elog_end" not in symbols:
        raise ValueError("missing logger linker symbols")
    log_start, log_end = symbols["__elog_start"], symbols["__elog_end"]
    if (log_start != expected_log_start or
            log_end != expected_log_start + profile["log_bytes"] or
            log_end != flash_end or
            (log_start - FLASH_BASE) % profile["page_bytes"] or
            (log_end - log_start) % profile["page_bytes"]):
        raise ValueError("logger region has invalid device bounds or alignment")
    highest_loaded = FLASH_BASE
    vector_loaded = False
    for index in range(phnum):
        segment = struct.unpack_from("<IIIIIIII", data, phoff + phentsize * index)
        kind, offset, _, physical, file_size = segment[:5]
        if kind != 1 or file_size == 0:
            continue
        if offset + file_size > len(data):
            raise ValueError("loadable segment exceeds ELF size")
        end = physical + file_size
        if physical < FLASH_ADDRESS_SPACE_END and end > FLASH_BASE:
            if physical < FLASH_BASE or end > log_start:
                raise ValueError(f"loadable firmware overlaps logger at {physical:#x}..{end:#x}")
            vector_loaded |= physical <= FLASH_BASE < end
            highest_loaded = max(highest_loaded, end)
    if not vector_loaded:
        raise ValueError("interrupt vectors are missing from loadable firmware")
    for section in sections:
        _, _, flags, address, _, size = section[:6]
        if (flags & 2 and size and address < FLASH_ADDRESS_SPACE_END
                and address + size > FLASH_BASE):
            if address < FLASH_BASE or address + size > log_start:
                raise ValueError(f"allocated section overlaps logger at {address:#x}")
    return {"firmware_loaded_end": highest_loaded,
            "log_start": log_start, "log_end": log_end}


def main():
    """Read an ELF path and target profile, then print verified boundaries."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--target", choices=TARGETS, default="rd")
    args = parser.parse_args()
    result = verify(args.elf, args.target)
    print("Flash layout verified: firmware ends by "
          f"{result['firmware_loaded_end']:#010x}; logger "
          f"{result['log_start']:#010x}..{result['log_end']:#010x}")


if __name__ == "__main__":
    main()
