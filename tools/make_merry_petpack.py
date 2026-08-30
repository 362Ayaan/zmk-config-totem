#!/usr/bin/env python3
"""Convert a Codex v1 pet atlas into the compact dongle pet-pack format.

The input atlas is 8 columns by 9 rows of 192x208 RGBA cells. The five Codex-
ready output animations retain that native resolution and use stable 63-colour
RGB565 palettes; palette index zero is transparent/black. Indices are packed
as a continuous 6-bit stream.
"""

from __future__ import annotations

import argparse
import binascii
import colorsys
import json
import struct
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter


ATLAS_COLUMNS = 8
ATLAS_ROWS = 9
CELL_WIDTH = 192
CELL_HEIGHT = 208
FRAME_WIDTH = 192
FRAME_HEIGHT = 208
ALPHA_THRESHOLD = 32
DEFAULT_SATURATION = 1.18
DEFAULT_COOL_BOOST = 1.12
DEFAULT_RESAMPLING = "native"

PACK_MAGIC = 0x31544550  # PET1 in little endian
PACK_VERSION = 4
BITS_PER_PIXEL = 6
PALETTE_SIZE = 1 << BITS_PER_PIXEL
VISIBLE_COLORS = PALETTE_SIZE - 1
ANCHOR_COLORS = (
    (22, 199, 255),  # bright cyan water/highlight
    (8, 125, 209),   # saturated mid-blue water/shadow
    (66, 206, 114),  # emerald foliage
    (20, 126, 72),   # deep green foliage/shadow
    (244, 238, 228), # warm off-white sails/body
    (33, 17, 36),    # dark plum outline that remains distinct from black
)
ADAPTIVE_COLORS = VISIBLE_COLORS - len(ANCHOR_COLORS)
PACK_HEADER = struct.Struct("<IHHHHHHIIIIII")
ANIMATION_DESC = struct.Struct("<BBHHH")
FRAME_DESC = struct.Struct(f"<IHH{PALETTE_SIZE}H")


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


def image_pixels(image: Image.Image) -> list[tuple[int, ...]]:
    getter = getattr(image, "get_flattened_data", image.getdata)
    return list(getter())


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
    if 0.20 <= hue < 0.48 and sat > 0.18:
        # Pull muted olive foliage gently toward emerald, while retaining
        # light/dark structure for the two dedicated green anchors.
        hue = hue * 0.72 + 0.38 * 0.28
        sat = min(1.0, max(0.68, sat * cool_boost))
        value = min(1.0, value * (1.0 + (cool_boost - 1.0) * 1.5))
    elif 0.48 <= hue <= 0.72 and sat > 0.18:
        # Water benefits from a higher saturation floor on an illuminated IPS
        # panel, where dark cyan otherwise reads as grey at full backlight.
        sat = min(1.0, max(0.76, sat * cool_boost))
        value = min(1.0, value * (1.0 + (cool_boost - 1.0) * 1.35))
    boosted = colorsys.hsv_to_rgb(hue, sat, value)
    return tuple((round(channel * 255) * a + 127) // 255 for channel in boosted)


def rgb_to_oklab(rgb: tuple[int, int, int]) -> tuple[float, float, float]:
    """Convert sRGB to OKLab for hue-stable offline palette matching."""
    linear = []
    for channel in rgb:
        value = channel / 255.0
        linear.append(value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4)
    red, green, blue = linear
    light = 0.4122214708 * red + 0.5363325363 * green + 0.0514459929 * blue
    medium = 0.2119034982 * red + 0.6806995451 * green + 0.1073969566 * blue
    short = 0.0883024619 * red + 0.2817188376 * green + 0.6299787005 * blue
    light_root = light ** (1.0 / 3.0)
    medium_root = medium ** (1.0 / 3.0)
    short_root = short ** (1.0 / 3.0)
    return (
        0.2104542553 * light_root + 0.7936177850 * medium_root - 0.0040720468 * short_root,
        1.9779984951 * light_root - 2.4285922050 * medium_root + 0.4505937099 * short_root,
        0.0259040371 * light_root + 0.7827717662 * medium_root - 0.8086757660 * short_root,
    )


def oklab_distance(left: tuple[float, float, float], right: tuple[float, float, float]) -> float:
    return sum((left[channel] - right[channel]) ** 2 for channel in range(3))


def resize_frame(frame: Image.Image, resampling: str) -> Image.Image:
    if resampling == "native":
        if frame.size != (FRAME_WIDTH, FRAME_HEIGHT):
            raise ValueError(f"native frame is {frame.size}, expected {(FRAME_WIDTH, FRAME_HEIGHT)}")
        return frame.copy()
    if resampling == "nearest":
        return frame.resize((FRAME_WIDTH, FRAME_HEIGHT), Image.Resampling.NEAREST)
    resized = frame.resize((FRAME_WIDTH, FRAME_HEIGHT), Image.Resampling.BOX)
    alpha = resized.getchannel("A")
    sharpened = resized.convert("RGB").filter(
        ImageFilter.UnsharpMask(radius=0.65, percent=65, threshold=3)
    ).convert("RGBA")
    sharpened.putalpha(alpha)
    return sharpened


def build_animation_palette(
    frames: list[Image.Image], saturation: float, cool_boost: float
) -> list[int]:
    visible = []
    for frame in frames:
        visible.extend(
            enhance_pixel(px, saturation, cool_boost)
            for px in image_pixels(frame.convert("RGBA"))
            if px[3] >= ALPHA_THRESHOLD
        )
    visible = [
        color for color in visible if max(color) >= 4
    ]
    if not visible:
        raise ValueError("animation is empty")

    sample = Image.new("RGB", (len(visible), 1))
    sample.putdata(visible)
    quantized = sample.quantize(
        colors=ADAPTIVE_COLORS, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE
    )
    raw_palette = quantized.getpalette() or []
    used_indices = sorted(index for _, index in (quantized.getcolors() or []))
    adaptive_rgb = [
        tuple(raw_palette[index * 3 : index * 3 + 3])  # type: ignore[arg-type]
        for index in used_indices[:ADAPTIVE_COLORS]
    ]

    # Put the anchor colors first, then deduplicate after RGB565 conversion.
    # Repeating the last valid entry for unused slots avoids creating opaque
    # black candidates that could erase dark-but-visible outline pixels.
    visible_palette565: list[int] = []
    for color in (*ANCHOR_COLORS, *adaptive_rgb):
        encoded = rgb888_to_rgb565(color)
        if encoded != 0 and encoded not in visible_palette565:
            visible_palette565.append(encoded)
    while len(visible_palette565) < VISIBLE_COLORS:
        visible_palette565.append(visible_palette565[-1])
    return [0] + visible_palette565[:VISIBLE_COLORS]


def quantize_frame(
    frame: Image.Image,
    palette565: list[int],
    saturation: float,
    cool_boost: float,
) -> tuple[list[int], bytes, Image.Image]:
    rgba = frame.convert("RGBA")
    pixels = image_pixels(rgba)
    palette_display = [rgb565_to_rgb888(value) for value in palette565]
    palette_oklab = [rgb_to_oklab(color) for color in palette_display]
    nearest_cache: dict[tuple[int, int, int], int] = {}
    indices: list[int] = []

    for px in pixels:
        if px[3] < ALPHA_THRESHOLD:
            indices.append(0)
            continue

        rgb = enhance_pixel(px, saturation, cool_boost)
        index = nearest_cache.get(rgb)
        if index is None:
            source_oklab = rgb_to_oklab(rgb)
            index = min(
                range(1, PALETTE_SIZE),
                key=lambda candidate: oklab_distance(source_oklab, palette_oklab[candidate]),
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
    fallback_size = (FRAME_WIDTH * FRAME_HEIGHT * BITS_PER_PIXEL + 7) // 8
    header.write_text(
        f"""/* Generated by tools/make_merry_petpack.py. */
#pragma once

#include <stdint.h>

#define MERRY_FALLBACK_WIDTH {FRAME_WIDTH}
#define MERRY_FALLBACK_HEIGHT {FRAME_HEIGHT}
#define MERRY_FALLBACK_DATA_SIZE {fallback_size}

extern const uint16_t merry_fallback_palette[{PALETTE_SIZE}];
extern const uint8_t merry_fallback_pixels[MERRY_FALLBACK_DATA_SIZE];
""",
        encoding="utf-8",
    )
    palette_text = ", ".join(f"0x{value:04x}" for value in palette)
    source.write_text(
        "/* Generated by tools/make_merry_petpack.py. */\n"
        '#include "merry_fallback.h"\n\n'
        f"const uint16_t merry_fallback_palette[{PALETTE_SIZE}] = {{{palette_text}}};\n\n"
        + c_byte_array("merry_fallback_pixels", pixels),
        encoding="utf-8",
    )


def build_pack(
    atlas_path: Path,
    output_dir: Path,
    saturation: float = DEFAULT_SATURATION,
    cool_boost: float = DEFAULT_COOL_BOOST,
    resampling: str = DEFAULT_RESAMPLING,
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
        source_frames = [
            resize_frame(
                atlas.crop(
                    (
                        column * CELL_WIDTH,
                        animation.source_row * CELL_HEIGHT,
                        (column + 1) * CELL_WIDTH,
                        (animation.source_row + 1) * CELL_HEIGHT,
                    )
                ),
                resampling,
            )
            for column in range(animation.count)
        ]
        animation_palette = build_animation_palette(source_frames, saturation, cool_boost)
        for column in range(animation.count):
            palette, pixels, preview = quantize_frame(
                source_frames[column], animation_palette, saturation, cool_boost
            )
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

    contact = Image.new(
        "RGB", (ATLAS_COLUMNS * FRAME_WIDTH, len(ANIMATIONS) * FRAME_HEIGHT), "#101015"
    )
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
        "palette_scope": "animation",
        "anchor_colors": ["#{:02x}{:02x}{:02x}".format(*color) for color in ANCHOR_COLORS],
        "quantization_distance": "oklab-euclidean",
        "saturation": saturation,
        "blue_green_boost": cool_boost,
        "resampling": resampling,
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
    parser.add_argument(
        "--resampling", choices=("native", "nearest", "box-sharp"), default=DEFAULT_RESAMPLING
    )
    args = parser.parse_args()
    if not 1.0 <= args.saturation <= 1.5:
        parser.error("--saturation must be between 1.0 and 1.5")
    if not 1.0 <= args.cool_boost <= 1.25:
        parser.error("--cool-boost must be between 1.0 and 1.25")
    manifest = build_pack(
        args.atlas, args.output_dir, args.saturation, args.cool_boost, args.resampling
    )
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
