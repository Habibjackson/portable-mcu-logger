"""Exercise both STM32 target profiles and reject unsafe ELF layouts."""

import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from verify_flash_layout import verify


def fixture(load_address, log_start, log_end, vector_address=0x08000000):
    """Construct a minimal ELF32 image with configurable flash boundaries."""
    image = bytearray(512)
    image[:6] = b"\x7fELF\x01\x01"
    struct.pack_into("<HHIIIIIHHHHHH", image, 16,
                     2, 40, 1, 0x08000008, 52, 84, 0, 52, 32, 1, 40, 5, 4)
    struct.pack_into("<IIIIIIII", image, 52,
                     1, 320, load_address, load_address, 16, 16, 5, 4)
    # Null, vector, symbol, symbol strings, and section-name strings.
    struct.pack_into("<IIIIIIIIII", image, 124,
                     1, 1, 2, vector_address, 320, 16, 0, 0, 4, 0)
    struct.pack_into("<IIIIIIIIII", image, 164,
                     0, 2, 0, 0, 352, 48, 3, 0, 4, 16)
    names = b"\0__elog_start\0__elog_end\0"
    struct.pack_into("<IIIIIIIIII", image, 204,
                     0, 3, 0, 0, 400, len(names), 0, 0, 1, 0)
    section_names = b"\0.isr_vector\0.shstrtab\0"
    struct.pack_into("<IIIIIIIIII", image, 244,
                     section_names.index(b".shstrtab"), 3, 0, 0, 430,
                     len(section_names), 0, 0, 1, 0)
    image[400:400 + len(names)] = names
    image[430:430 + len(section_names)] = section_names
    struct.pack_into("<II", image, 368, names.index(b"__elog_start"), log_start)
    struct.pack_into("<II", image, 384, names.index(b"__elog_end"), log_end)
    return image


def expect_failure(path, image, target, message):
    """Require an unsafe fixture to fail with the expected diagnostic."""
    path.write_bytes(image)
    try:
        verify(path, target)
    except ValueError as exc:
        assert message in str(exc), exc
    else:
        raise AssertionError(f"unsafe {target} image passed: {message}")


def main():
    """Check valid RD/C8 layouts and representative overlap failures."""
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "fixture.elf"
        for target, start, end in (("rd", 0x08058000, 0x08060000),
                                   ("c8", 0x0800E000, 0x08010000)):
            path.write_bytes(fixture(0x08000000, start, end))
            result = verify(path, target)
            assert result["log_start"] == start
            assert result["log_end"] == end
            expect_failure(path, fixture(start, start, end), target,
                           "overlaps logger")
            expect_failure(path, fixture(end, start, end), target,
                           "overlaps logger")
            expect_failure(path, fixture(0x08000000, start - 1024, end),
                           target, "invalid device bounds")
            expect_failure(path, fixture(0x08000000, start, end,
                                         0x08000400), target,
                           "interrupt vectors must start")
    print("ELF reserved-flash verifier: RD and C8 OK")


if __name__ == "__main__":
    main()
