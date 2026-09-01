#!/usr/bin/env python3
"""Build the full-colour ESP32-S3 Merry animation pack.

The source is the original 8x9 Codex pet atlas. Selected animations are scaled
exactly 1.125x with nearest-neighbour sampling and stored as complete RGB565
frames. The ESP can therefore memcpy a frame from PSRAM without runtime image
decoding, palette lookup, scaling, or transient allocations.
"""

from __future__ import annotations

import argparse
import binascii
import colorsys
import json
import struct
from dataclasses import dataclass
from pathlib import Path

from PIL import Image


ATLAS_COLUMNS = 8
ATLAS_ROWS = 9
CELL_WIDTH = 192
CELL_HEIGHT = 208
FRAME_WIDTH = 216
FRAME_HEIGHT = 234

PACK_MAGIC = 0x3546504D  # MPF5
PACK_VERSION = 1
HEADER = struct.Struct("<IHHHHHHIIIII7I")
ANIMATION_DESC = struct.Struct("<BBHHH")
FRAME_DESC = struct.Struct("<IHH")


@dataclass(frozen=True)
class Animation:
    name: str
    source_row: int
    count: int
    durations_ms: tuple[int, ...]
    loop: bool


ANIMATIONS = (
    Animation("idle", 0, 6, (280, 110, 110, 140, 140, 320), True),
    Animation("running", 7, 6, (120, 120, 120, 120, 120, 220), True),
    Animation("needs-input", 6, 6, (150, 150, 150, 150, 150, 260), True),
    Animation("completed", 3, 4, (140, 140, 140, 280), False),
    Animation("blocked", 5, 8, (140, 140, 140, 140, 140, 140, 140, 240), False),
)


def enhance(red: int, green: int, blue: int, alpha: int) -> tuple[int, int, int]:
    """Apply the restrained IPS-oriented colour treatment used in V1."""
    if alpha < 32:
        return (0, 0, 0)
    hue, saturation, value = colorsys.rgb_to_hsv(red / 255, green / 255, blue / 255)
    saturation = min(1.0, saturation * 1.18)
    if 0.20 <= hue < 0.48 and saturation > 0.18:
        hue = hue * 0.72 + 0.38 * 0.28
        saturation = min(1.0, max(0.68, saturation * 1.12))
        value = min(1.0, value * 1.18)
    elif 0.48 <= hue <= 0.72 and saturation > 0.18:
        saturation = min(1.0, max(0.76, saturation * 1.12))
        value = min(1.0, value * 1.162)
    channels = colorsys.hsv_to_rgb(hue, saturation, value)
    # Alpha-composite antialiased edge pixels against the intended black panel.
    return tuple((round(channel * 255) * alpha + 127) // 255 for channel in channels)


def rgb565(red: int, green: int, blue: int) -> int:
    return ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)


def frame_bytes(frame: Image.Image) -> bytes:
    pixels = frame.convert("RGBA").get_flattened_data()
    output = bytearray(FRAME_WIDTH * FRAME_HEIGHT * 2)
    for index, (red, green, blue, alpha) in enumerate(pixels):
        encoded = rgb565(*enhance(red, green, blue, alpha))
        struct.pack_into("<H", output, index * 2, encoded)
    return bytes(output)


def build(source: Path, destination: Path, manifest_path: Path) -> None:
    atlas = Image.open(source).convert("RGBA")
    expected = (ATLAS_COLUMNS * CELL_WIDTH, ATLAS_ROWS * CELL_HEIGHT)
    if atlas.size != expected:
        raise ValueError(f"expected atlas {expected}, got {atlas.size}")

    animations: list[tuple[int, int, int, int, int]] = []
    frames: list[tuple[int, bytes]] = []
    first_frame = 0
    for animation_id, animation in enumerate(ANIMATIONS):
        animations.append(
            (animation_id, 1 if animation.loop else 0, first_frame, animation.count, 0)
        )
        for column in range(animation.count):
            crop = atlas.crop(
                (
                    column * CELL_WIDTH,
                    animation.source_row * CELL_HEIGHT,
                    (column + 1) * CELL_WIDTH,
                    (animation.source_row + 1) * CELL_HEIGHT,
                )
            )
            resized = crop.resize((FRAME_WIDTH, FRAME_HEIGHT), Image.Resampling.NEAREST)
            frames.append((animation.durations_ms[column], frame_bytes(resized)))
        first_frame += animation.count

    animation_offset = HEADER.size
    frame_offset = animation_offset + len(animations) * ANIMATION_DESC.size
    data_offset = frame_offset + len(frames) * FRAME_DESC.size
    frame_size = FRAME_WIDTH * FRAME_HEIGHT * 2
    animation_table = b"".join(ANIMATION_DESC.pack(*entry) for entry in animations)
    frame_table = b"".join(
        FRAME_DESC.pack(data_offset + index * frame_size, duration, 0)
        for index, (duration, _) in enumerate(frames)
    )
    pixel_data = b"".join(pixels for _, pixels in frames)
    body = animation_table + frame_table + pixel_data
    total_size = HEADER.size + len(body)
    content_crc = binascii.crc32(body) & 0xFFFFFFFF
    header = HEADER.pack(
        PACK_MAGIC,
        PACK_VERSION,
        HEADER.size,
        FRAME_WIDTH,
        FRAME_HEIGHT,
        len(frames),
        len(animations),
        animation_offset,
        frame_offset,
        data_offset,
        total_size,
        content_crc,
        0, 0, 0, 0, 0, 0, 0,
    )
    pack = header + body
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(pack)
    manifest = {
        "source": str(source),
        "format": "MPF5",
        "version": PACK_VERSION,
        "frame_width": FRAME_WIDTH,
        "frame_height": FRAME_HEIGHT,
        "frame_count": len(frames),
        "animation_count": len(animations),
        "pixel_format": "RGB565 little-endian",
        "bytes_per_frame": frame_size,
        "pack_bytes": len(pack),
        "pack_crc32": f"{binascii.crc32(pack) & 0xFFFFFFFF:08x}",
        "content_crc32": f"{content_crc:08x}",
        "animations": [
            {
                "id": index,
                "name": animation.name,
                "frames": animation.count,
                "loop": animation.loop,
                "durations_ms": list(animation.durations_ms),
            }
            for index, animation in enumerate(ANIMATIONS)
        ],
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("manifest", type=Path)
    args = parser.parse_args()
    build(args.source, args.destination, args.manifest)


if __name__ == "__main__":
    main()
