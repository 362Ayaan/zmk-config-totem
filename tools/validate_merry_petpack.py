#!/usr/bin/env python3
"""Strict structural and CRC validation for the dongle Merry pet pack."""

from __future__ import annotations

import argparse
import binascii
import json
import struct
from pathlib import Path


PACK_MAGIC = 0x31544550
PACK_VERSION = 3
FRAME_WIDTH = 192
FRAME_HEIGHT = 208
BITS_PER_PIXEL = 6
PALETTE_SIZE = 1 << BITS_PER_PIXEL
SLOT_PAYLOAD_BYTES = 1024 * 1024 - 4096

PACK_HEADER = struct.Struct("<IHHHHHHIIIIII")
ANIMATION_DESC = struct.Struct("<BBHHH")
FRAME_DESC = struct.Struct(f"<IHH{PALETTE_SIZE}H")
FRAME_BYTES = (FRAME_WIDTH * FRAME_HEIGHT * BITS_PER_PIXEL + 7) // 8


def validate(pack_path: Path, manifest_path: Path) -> None:
    pack = pack_path.read_bytes()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if len(pack) > SLOT_PAYLOAD_BYTES:
        raise ValueError(f"pack exceeds atomic slot by {len(pack) - SLOT_PAYLOAD_BYTES} bytes")
    if len(pack) < PACK_HEADER.size:
        raise ValueError("pack is shorter than its header")

    (
        magic,
        version,
        header_size,
        width,
        height,
        frame_count,
        animation_count,
        animation_offset,
        frame_offset,
        data_offset,
        total_size,
        content_crc,
        reserved,
    ) = PACK_HEADER.unpack_from(pack)

    expected_header = (PACK_MAGIC, PACK_VERSION, PACK_HEADER.size, FRAME_WIDTH, FRAME_HEIGHT)
    if (magic, version, header_size, width, height) != expected_header:
        raise ValueError("header identity, version, dimensions, or size is invalid")
    if reserved != 0 or frame_count == 0 or animation_count == 0 or total_size != len(pack):
        raise ValueError("header counts, reserved field, or total size is invalid")
    if animation_offset != PACK_HEADER.size:
        raise ValueError("animation table is not directly after the header")
    if frame_offset != animation_offset + animation_count * ANIMATION_DESC.size:
        raise ValueError("frame table is not directly after the animation table")
    if data_offset != frame_offset + frame_count * FRAME_DESC.size:
        raise ValueError("pixel data is not directly after the frame table")
    if data_offset + frame_count * FRAME_BYTES != total_size:
        raise ValueError("pixel data does not exactly fill the declared pack")
    if binascii.crc32(pack[PACK_HEADER.size :]) & 0xFFFFFFFF != content_crc:
        raise ValueError("content CRC32 mismatch")

    for index in range(animation_count):
        animation = ANIMATION_DESC.unpack_from(pack, animation_offset + index * ANIMATION_DESC.size)
        animation_id, _flags, first_frame, count, animation_reserved = animation
        if animation_id != index or animation_reserved != 0 or count == 0:
            raise ValueError(f"animation {index} descriptor is invalid")
        if first_frame >= frame_count or count > frame_count - first_frame:
            raise ValueError(f"animation {index} frame range is invalid")

    for index in range(frame_count):
        frame = FRAME_DESC.unpack_from(pack, frame_offset + index * FRAME_DESC.size)
        pixel_offset, duration_ms, frame_reserved, *palette = frame
        if pixel_offset != data_offset + index * FRAME_BYTES:
            raise ValueError(f"frame {index} pixel data is not contiguous")
        if not 20 <= duration_ms <= 10_000 or frame_reserved != 0 or palette[0] != 0:
            raise ValueError(f"frame {index} duration, reserved field, or transparent color is invalid")

        # Mirror the firmware's streaming 6-bit decoder to prove the pack can
        # be consumed without an overread and contains both transparent and
        # visible pixels.
        packed = pack[pixel_offset : pixel_offset + FRAME_BYTES]
        packed_offset = 0
        bit_buffer = 0
        buffered_bits = 0
        saw_transparent = False
        saw_visible = False
        for _pixel in range(FRAME_WIDTH * FRAME_HEIGHT):
            while buffered_bits < BITS_PER_PIXEL:
                bit_buffer |= packed[packed_offset] << buffered_bits
                packed_offset += 1
                buffered_bits += 8
            palette_index = bit_buffer & (PALETTE_SIZE - 1)
            bit_buffer >>= BITS_PER_PIXEL
            buffered_bits -= BITS_PER_PIXEL
            saw_transparent |= palette_index == 0
            saw_visible |= palette_index != 0
        if packed_offset != FRAME_BYTES or buffered_bits != 0 or bit_buffer != 0:
            raise ValueError(f"frame {index} decoder did not consume its payload exactly")
        if not saw_transparent or not saw_visible:
            raise ValueError(f"frame {index} does not contain both transparent and visible pixels")

    whole_crc = binascii.crc32(pack) & 0xFFFFFFFF
    expected_manifest = {
        "format_version": PACK_VERSION,
        "frame_width": FRAME_WIDTH,
        "frame_height": FRAME_HEIGHT,
        "frame_count": frame_count,
        "animation_count": animation_count,
        "palette_colors_per_frame": PALETTE_SIZE - 1,
        "bits_per_pixel": BITS_PER_PIXEL,
        "packed_bytes_per_frame": FRAME_BYTES,
        "pack_bytes": len(pack),
        "pack_crc32": f"{whole_crc:08x}",
        "content_crc32": f"{content_crc:08x}",
    }
    for key, expected in expected_manifest.items():
        if manifest.get(key) != expected:
            raise ValueError(f"manifest {key!r} is {manifest.get(key)!r}, expected {expected!r}")

    print(
        f"Merry pack OK: {len(pack):,} bytes, {frame_count} frames, "
        f"{animation_count} animations, {SLOT_PAYLOAD_BYTES - len(pack):,} bytes slot headroom, "
        f"CRC32 {whole_crc:08x}"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("pack", type=Path, nargs="?", default=Path("assets/merry/merry.petpack"))
    parser.add_argument(
        "--manifest", type=Path, default=Path("assets/merry/merry.petpack.json")
    )
    args = parser.parse_args()
    validate(args.pack, args.manifest)


if __name__ == "__main__":
    main()
