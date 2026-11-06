#!/usr/bin/env python3
"""Check the fixed RAM aperture, executable size and required stack reserve."""
from pathlib import Path
import json
import re
import sys


def main():
    map_file = Path(sys.argv[1])
    binary_file = Path(sys.argv[2])
    match = re.search(r"^\s*([0-9a-fA-F]+)\s+s__HEAP\b", map_file.read_text(), re.M)
    if not match:
        raise SystemExit("Missing heap-end linker symbol")
    static_end = int(match[1], 16)
    binary_bytes = binary_file.stat().st_size
    assert 0x4000 <= static_end <= 0xC000, (
        "Static data/code overlaps page-3 VRAM aperture", static_end
    )
    if binary_file.name == "zdungeon_test":
        assert static_end <= 0xD000, ("Stack painting would overlap static storage", static_end)
    if binary_file.name == "zdungeon":
        assert 0xC000 - static_end >= 2048, (
            "Require2048bytes below the VRAM aperture", 0xC000 - static_end
        )
    assert binary_bytes <= 32768, ("Binary exceeds 32 KiB", binary_bytes)
    print(json.dumps(dict(
        binary_bytes=binary_bytes,
        static_end=static_end,
        address_headroom=65536 - static_end,
        required_reserve=16384,
        static_to_aperture=0xC000 - static_end,
    )))


if __name__ == "__main__":
    main()
