import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    demo = Path(sys.argv[1])
    root = Path(sys.argv[2])
    sys.path.insert(0, str(root / "tools"))
    from decode_logs import parse_image, regions_from_args
    from estimate_flash_lifetime import estimate
    from uart_hex_to_bin import extract

    with tempfile.TemporaryDirectory() as folder:
        image_path = Path(folder) / "image.bin"
        subprocess.run([str(demo), str(image_path)], check=True, capture_output=True)
        image = image_path.read_bytes()
        regions = regions_from_args(len(image), 2048, None)
        records, warnings = parse_image(image, regions, 2)
        assert not warnings, warnings
        assert len(records) == 13, len(records)
        assert records[1]["codec"] == 1
        assert records[1]["numeric_i32"][:4] == [1001, 121, 206, 9001]
        assert records[6]["type"] == 3
        transcript = ("console status\nELOG_HEX_BEGIN 32768\n" +
                      "\n".join(image[i:i + 16].hex().upper()
                                for i in range(0, len(image), 16)) +
                      "\nELOG_HEX_END\n")
        assert extract(transcript) == image
        try:
            extract(transcript.replace("ELOG_HEX_END", "ELOG_HEX_ERROR"))
        except ValueError as exc:
            assert "invalid dump line" in str(exc)
        else:
            raise AssertionError("truncated UART dump was accepted")
        human = subprocess.run(
            [sys.executable, str(root / "tools" / "decode_logs.py"),
             str(image_path), "--schema", str(root / "examples" / "robot_schema.json"),
             "--human"], check=True, capture_output=True, text=True).stdout
        assert "motor_speed=1000 rpm" in human
        assert "motor_current=0.12 A" in human
        assert "motor_overcurrent" in human
        assert "duration=500 ms" in human
        assert "motor_stopped" in human
        blank = next(start for start, length in regions
                     if image[start:start + length] == b"\xff" * length)
        partial = bytearray(image)
        partial[blank + 40] = 0
        _, warnings = parse_image(partial, regions, 2)
        assert any("erased header with programmed body" in item for item in warnings)
    result = estimate(10, 122, 2048, 8, 10000, 1, 1, 32)
    assert abs(result["idealized_lifetime_days"] - 1.48148) < 0.001
    assert result["records_per_day"] == 864000
    print("host decoder and endurance calculator: OK")


if __name__ == "__main__":
    main()
