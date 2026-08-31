/* SPDX-License-Identifier: MIT */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define MERRY_MEDIA_WIDTH 202u
#define MERRY_MEDIA_HEIGHT 220u
#define MERRY_MEDIA_BYTES (MERRY_MEDIA_WIDTH * MERRY_MEDIA_HEIGHT * sizeof(uint16_t))

enum merry_media_state {
    MERRY_MEDIA_NONE = 0,
    MERRY_MEDIA_PLAYING = 1,
    MERRY_MEDIA_PAUSED = 2,
};

uint8_t merry_media_state_get(void);
int merry_media_state_set(uint8_t state);
int merry_media_upload_begin(void);
int merry_media_upload_write(size_t offset, const uint8_t *data, size_t size);
int merry_media_upload_finish(uint32_t expected_crc32, uint8_t state);
void merry_media_upload_abort(void);

