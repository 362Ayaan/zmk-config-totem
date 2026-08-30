/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/toolchain.h>

#define PET_PACK_MAGIC 0x31544550u /* PET1 */
#define PET_PACK_VERSION 3u
#define PET_FRAME_WIDTH 192u
#define PET_FRAME_HEIGHT 208u
#define PET_BITS_PER_PIXEL 6u
#define PET_FRAME_PACKED_BYTES                                                                  \
    ((PET_FRAME_WIDTH * PET_FRAME_HEIGHT * PET_BITS_PER_PIXEL + 7u) / 8u)
#define PET_PALETTE_SIZE (1u << PET_BITS_PER_PIXEL)
#define PET_MAX_ANIMATIONS 16u
#define PET_MAX_FRAMES 128u

enum merry_animation_id {
    MERRY_ANIM_IDLE = 0,
    MERRY_ANIM_RUNNING,
    MERRY_ANIM_NEEDS_INPUT,
    MERRY_ANIM_COMPLETED,
    MERRY_ANIM_BLOCKED,
};

struct pet_pack_header {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint16_t frame_width;
    uint16_t frame_height;
    uint16_t frame_count;
    uint16_t animation_count;
    uint32_t animation_table_offset;
    uint32_t frame_table_offset;
    uint32_t data_offset;
    uint32_t total_size;
    uint32_t content_crc32;
    uint32_t reserved;
} __packed;

struct pet_animation_desc {
    uint8_t id;
    uint8_t flags;
    uint16_t first_frame;
    uint16_t frame_count;
    uint16_t reserved;
} __packed;

struct pet_frame_desc {
    uint32_t data_offset;
    uint16_t duration_ms;
    uint16_t reserved;
    uint16_t palette[PET_PALETTE_SIZE];
} __packed;

struct pet_frame {
    uint16_t duration_ms;
    uint16_t palette[PET_PALETTE_SIZE];
};
