#!/usr/bin/env python3
"""Convert a Codex v1 pet atlas into the compact dongle pet-pack format.

The input atlas is 8 columns by 9 rows of 192x208 RGBA cells. Each output
frame is 160x174 pixels with a private 31-colour RGB565 palette; palette index
zero is transparent/black. Indices are packed as a continuous 5-bit stream.
"""

from __future__ import annotations

import argparse
import binascii
import colorsys
import json
import struct
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw


ATLAS_COLUMNS = 8
ATLAS_ROWS = 9
CELL_WIDTH = 192
CELL_HEIGHT = 208
FRAME_WIDTH = 160
FRAME_HEIGHT = 174
ALPHA_THRESHOLD = 32
DEFAULT_SATURATION = 1.14
DEFAULT_COOL_BOOST = 1.06

PACK_MAGIC = 0x31544550  # PET1 in little endian
PACK_VERSION = 2
BITS_PER_PIXEL = 5
PALETTE_SIZE = 1 << BITS_PER_PIXEL
VISIBLE_COLORS = PALETTE_SIZE - 1
PACK_HEADER = struct.Struct("<IHHHHHHIIIIII")
ANIMATION_DESC = struct.Struct("<BBHHH")
FRAME_DESC = struct.Struct(f"<IHH{PALETTE_SIZE}H")


@dataclass(frozen=True)
class Animation:
    name: str
    count: int
    durations_ms: tuple[int, ...]
    loop: bool


ANIMATIONS = (
    Animation("idle", 6, (280, 110, 110, 140, 140, 320), True),
    Animation("running-right", 8, (120, 120, 120, 120, 120, 120, 120, 220), True),
    Animation("running-left", 8, (120, 120, 120, 120, 120, 120, 120, 220), True),
    Animation("waving", 4, (140, 140, 140, 280), False),
    Animation("jumping", 5, (140, 140, 140, 140, 280), False),
    Animation("failed", 8, (140, 140, 140, 140, 140, 140, 140, 240), False),
    Animation("waiting", 6, (150, 150, 150, 150, 150, 260), True),
    Animation("running", 6, (120, 120, 120, 120, 120, 220), True),
    Animation("review", 6, (150, 150, 150, 150, 150, 280), True),
)


def rgb888_to_rgb565(rgb: tuple[int, int, int]) -> int:
    r, g, b = rgb
    return ((r * 31 + 127) // 255 << 11) | ((g * 63 + 127) // 255 << 5) | (
        (b * 31 + 127) // 255
    )


def rgb565_to_rgb888(value: int) -> tuple[int, int, int]:
    r = (value >> 11) & 0x1F
    g = (value >> 5) & 0x3F
    b = value & 0x1F
    return ((r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31)


def composite_on_black(pixel: tuple[int, int, int, int]) -> tuple[int, int, int]:
    r, g, b, a = pixel
    return ((r * a + 127) // 255, (g * a + 127) // 255, (b * a + 127) // 255)


def enhance_pixel(
    pixel: tuple[int, int, int, int], saturation: float, cool_boost: float
) -> tuple[int, int, int]:
    """Apply a restrained display-oriented boost before alpha compositing.

    The extra cool-range lift targets Merry's water and foliage. Applying it
    before RGB565 conversion keeps the runtime decoder and memory use unchanged.
    """
    r, g, b, a = pixel
    hue, sat, value = colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)
    sat = min(1.0, sat * saturation)
    if 0.25 <= hue <= 0.67 and sat > 0.18:
        sat = min(1.0, sat * cool_boost)
        value = min(1.0, value * cool_boost)
    boosted = colorsys.hsv_to_rgb(hue, sat, value)
    return tuple((round(channel * 255) * a + 127) // 255 for channel in boosted)


def perceptual_distance(left: tuple[int, int, int], right: tuple[int, int, int]) -> int:
    """Fast red-mean color distance, weighted for human-visible RGB errors."""
    red_mean = (left[0] + right[0]) // 2
    red = left[0] - right[0]
    green = left[1] - right[1]
    blue = left[2] - right[2]
    return (((512 + red_mean) * red * red) >> 8) + 4 * green * green + (
        ((767 - red_mean) * blue * blue) >> 8
    )


def quantize_frame(
    frame: Image.Image, saturation: float, cool_boost: float
) -> tuple[list[int], bytes, Image.Image]:
    rgba = frame.convert("RGBA")
    pixels = list(rgba.getdata())
    visible = [
        enhance_pixel(px, saturation, cool_boost)
        for px in pixels
        if px[3] >= ALPHA_THRESHOLD
    ]
    if not visible:
        raise ValueError("frame is empty")

    sample = Image.new("RGB", (len(visible), 1))
    sample.putdata(visible)
    quantized = sample.quantize(
        colors=VISIBLE_COLORS, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE
    )
    raw_palette = quantized.getpalette() or []
    used_indices = sorted(index for _, index in (quantized.getcolors() or []))
    palette_rgb = [
        tuple(raw_palette[index * 3 : index * 3 + 3])  # type: ignore[arg-type]
        for index in used_indices[:VISIBLE_COLORS]
    ]
    while len(palette_rgb) < VISIBLE_COLORS:
        palette_rgb.append((0, 0, 0))

    palette565 = [0] + [rgb888_to_rgb565(color) for color in palette_rgb]
    palette_display = [rgb565_to_rgb888(value) for value in palette565]
    nearest_cache: dict[tuple[int, int, int], int] = {}
    indices: list[int] = []

    for px in pixels:
        if px[3] < ALPHA_THRESHOLD:
            indices.append(0)
            continue

        rgb = enhance_pixel(px, saturation, cool_boost)
        index = nearest_cache.get(rgb)
        if index is None:
            index = min(
                range(1, PALETTE_SIZE),
                key=lambda candidate: perceptual_distance(rgb, palette_display[candidate]),
            )
            nearest_cache[rgb] = index
        indices.append(index)

    packed = bytearray()
    bit_buffer = 0
    buffered_bits = 0
    for index in indices:
        bit_buffer |= (index & (PALETTE_SIZE - 1)) << buffered_bits
        buffered_bits += BITS_PER_PIXEL
        while buffered_bits >= 8:
            packed.append(bit_buffer & 0xFF)
            bit_buffer >>= 8
            buffered_bits -= 8
    if buffered_bits:
        packed.append(bit_buffer & 0xFF)

    preview = Image.new("RGBA", (FRAME_WIDTH, FRAME_HEIGHT), (0, 0, 0, 0))
    preview.putdata(
        [
            (*palette_display[index], 255) if index else (0, 0, 0, 0)
            for index in indices
        ]
    )
    return palette565, bytes(packed), preview


def c_byte_array(name: str, data: bytes) -> str:
    lines = []
    for offset in range(0, len(data), 16):
        chunk = data[offset : offset + 16]
        lines.append("    " + ", ".join(f"0x{value:02x}" for value in chunk) + ",")
    return f"const uint8_t {name}[{len(data)}] = {{\n" + "\n".join(lines) + "\n};\n"


def write_fallback(output_dir: Path, palette: list[int], pixels: bytes) -> None:
    header = output_dir / "merry_fallback.h"
    source = output_dir / "merry_fallback.c"
    header.write_text(
        """/* Generated by tools/make_merry_petpack.py. */
#pragma once

#include <stdint.h>

#define MERRY_FALLBACK_WIDTH 160
#define MERRY_FALLBACK_HEIGHT 174
#define MERRY_FALLBACK_DATA_SIZE 17400

extern const uint16_t merry_fallback_palette[32];
extern const uint8_t merry_fallback_pixels[MERRY_FALLBACK_DATA_SIZE];
""",
        encoding="utf-8",
    )
    palette_text = ", ".join(f"0x{value:04x}" for value in palette)
    source.write_text(
        "/* Generated by tools/make_merry_petpack.py. */\n"
        '#include "merry_fallback.h"\n\n'
        f"const uint16_t merry_fallback_palette[32] = {{{palette_text}}};\n\n"
        + c_byte_array("merry_fallback_pixels", pixels),
        encoding="utf-8",
    )


def build_pack(
    atlas_path: Path,
    output_dir: Path,
    saturation: float = DEFAULT_SATURATION,
    cool_boost: float = DEFAULT_COOL_BOOST,
) -> dict[str, object]:
    atlas = Image.open(atlas_path).convert("RGBA")
    if atlas.size != (ATLAS_COLUMNS * CELL_WIDTH, ATLAS_ROWS * CELL_HEIGHT):
        raise ValueError(f"expected 1536x1872 atlas, got {atlas.size[0]}x{atlas.size[1]}")

    output_dir.mkdir(parents=True, exist_ok=True)
    preview_dir = output_dir / "qa"
    preview_dir.mkdir(exist_ok=True)

    frame_records: list[tuple[int, int, list[int], bytes, Image.Image, str, int]] = []
    first_frame = 0
    animation_records: list[tuple[int, int, int, int, int]] = []

    for animation_id, animation in enumerate(ANIMATIONS):
        animation_records.append((animation_id, 1 if animation.loop else 0, first_frame, animation.count, 0))
        gif_frames: list[Image.Image] = []
        for column in range(animation.count):
            cell = atlas.crop(
                (
                    column * CELL_WIDTH,
                    animation_id * CELL_HEIGHT,
                    (column + 1) * CELL_WIDTH,
                    (animation_id + 1) * CELL_HEIGHT,
                )
            ).resize((FRAME_WIDTH, FRAME_HEIGHT), Image.Resampling.NEAREST)
            palette, pixels, preview = quantize_frame(cell, saturation, cool_boost)
            duration = animation.durations_ms[column]
            frame_records.append((0, duration, palette, pixels, preview, animation.name, column))
            gif_frames.append(preview)

        gif_frames[0].save(
            preview_dir / f"{animation.name}.gif",
            save_all=True,
            append_images=gif_frames[1:],
            duration=list(animation.durations_ms),
            loop=0,
            disposal=2,
            transparency=0,
        )
        first_frame += animation.count

    animation_table_offset = PACK_HEADER.size
    frame_table_offset = animation_table_offset + len(animation_records) * ANIMATION_DESC.size
    data_offset = frame_table_offset + len(frame_records) * FRAME_DESC.size
    frame_size = (FRAME_WIDTH * FRAME_HEIGHT * BITS_PER_PIXEL + 7) // 8

    animation_table = b"".join(ANIMATION_DESC.pack(*record) for record in animation_records)
    frame_table_parts: list[bytes] = []
    pixel_parts: list[bytes] = []
    for index, (_, duration, palette, pixels, _, _, _) in enumerate(frame_records):
        if len(pixels) != frame_size:
            raise AssertionError("unexpected packed frame size")
        absolute_offset = data_offset + index * frame_size
        frame_table_parts.append(FRAME_DESC.pack(absolute_offset, duration, 0, *palette))
        pixel_parts.append(pixels)

    body = animation_table + b"".join(frame_table_parts) + b"".join(pixel_parts)
    total_size = PACK_HEADER.size + len(body)
    content_crc = binascii.crc32(body) & 0xFFFFFFFF
    header = PACK_HEADER.pack(
        PACK_MAGIC,
        PACK_VERSION,
        PACK_HEADER.size,
        FRAME_WIDTH,
        FRAME_HEIGHT,
        len(frame_records),
        len(animation_records),
        animation_table_offset,
        frame_table_offset,
        data_offset,
        total_size,
        content_crc,
        0,
    )
    pack = header + body
    (output_dir / "merry.petpack").write_bytes(pack)

    fallback = frame_records[0]
    write_fallback(output_dir, fallback[2], fallback[3])

    contact = Image.new("RGB", (ATLAS_COLUMNS * FRAME_WIDTH, ATLAS_ROWS * FRAME_HEIGHT), "#101015")
    draw = ImageDraw.Draw(contact)
    frame_cursor = 0
    for row, animation in enumerate(ANIMATIONS):
        for column in range(animation.count):
            preview = frame_records[frame_cursor][4]
            contact.paste(preview, (column * FRAME_WIDTH, row * FRAME_HEIGHT), preview)
            frame_cursor += 1
        draw.text((4, row * FRAME_HEIGHT + 4), animation.name, fill="white")
    contact.save(preview_dir / "contact-sheet.png")

    manifest = {
        "source": str(atlas_path),
        "source_url": "https://codex-pets.net/#/pets/merry",
        "attribution": "Merry by jeansolopreneur on codex-pets.net",
        "format_version": PACK_VERSION,
        "frame_width": FRAME_WIDTH,
        "frame_height": FRAME_HEIGHT,
        "frame_count": len(frame_records),
        "animation_count": len(animation_records),
        "palette_colors_per_frame": VISIBLE_COLORS,
        "quantization_distance": "redmean-perceptual",
        "saturation": saturation,
        "blue_green_boost": cool_boost,
        "bits_per_pixel": BITS_PER_PIXEL,
        "packed_bytes_per_frame": frame_size,
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
    (output_dir / "merry.petpack.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("atlas", type=Path)
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("--saturation", type=float, default=DEFAULT_SATURATION)
    parser.add_argument("--cool-boost", type=float, default=DEFAULT_COOL_BOOST)
    args = parser.parse_args()
    if not 1.0 <= args.saturation <= 1.5:
        parser.error("--saturation must be between 1.0 and 1.5")
    if not 1.0 <= args.cool_boost <= 1.25:
        parser.error("--cool-boost must be between 1.0 and 1.25")
    manifest = build_pack(args.atlas, args.output_dir, args.saturation, args.cool_boost)
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
