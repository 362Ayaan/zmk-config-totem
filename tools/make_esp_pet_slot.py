#!/usr/bin/env python3
"""Wrap an MPF5 pet pack in the ESP transactional slot header."""

from __future__ import annotations

import argparse
import binascii
import struct
from pathlib import Path


MAGIC = 0x3153504D  # MPS1
VERSION = 1
HEADER = struct.Struct("<IHHIII32s2II")
DATA_OFFSET = 4096


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("pack", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--pet-id", default="merry")
    parser.add_argument("--generation", type=int, default=1)
    args = parser.parse_args()

    pack = args.pack.read_bytes()
    pet_id = args.pet_id.encode("ascii", "strict")
    if not pet_id or len(pet_id) > 31:
        raise ValueError("pet id must be 1-31 ASCII bytes")
    header = HEADER.pack(
        MAGIC,
        VERSION,
        HEADER.size,
        args.generation,
        len(pack),
        binascii.crc32(pack) & 0xFFFFFFFF,
        pet_id.ljust(32, b"\0"),
        0,
        0,
        0,
    )
    header = header[:-4] + struct.pack("<I", binascii.crc32(header[:-4]) & 0xFFFFFFFF)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(header + bytes(DATA_OFFSET - len(header)) + pack)


if __name__ == "__main__":
    main()
