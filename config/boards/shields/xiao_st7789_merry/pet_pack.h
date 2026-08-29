/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/toolchain.h>

#define PET_PACK_MAGIC 0x31544550u /* PET1 */
#define PET_PACK_VERSION 1u
#define PET_FRAME_WIDTH 160u
#define PET_FRAME_HEIGHT 174u
#define PET_FRAME_PACKED_BYTES ((PET_FRAME_WIDTH * PET_FRAME_HEIGHT) / 2u)
#define PET_PALETTE_SIZE 16u
#define PET_MAX_ANIMATIONS 16u
#define PET_MAX_FRAMES 128u

enum merry_animation_id {
    MERRY_ANIM_IDLE = 0,
    MERRY_ANIM_RUNNING_RIGHT,
    MERRY_ANIM_RUNNING_LEFT,
    MERRY_ANIM_WAVING,
    MERRY_ANIM_JUMPING,
    MERRY_ANIM_FAILED,
    MERRY_ANIM_WAITING,
    MERRY_ANIM_RUNNING,
    MERRY_ANIM_REVIEW,
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

