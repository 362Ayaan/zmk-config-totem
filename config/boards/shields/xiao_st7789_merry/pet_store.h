/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pet_pack.h"

int pet_store_init(void);
bool pet_store_has_uploaded_pack(void);
int pet_store_get_animation(uint8_t animation_id, struct pet_animation_desc *animation);
int pet_store_read_frame(uint8_t animation_id, uint16_t animation_frame,
                         struct pet_frame *frame, uint8_t *packed_pixels,
                         size_t packed_pixels_size);

size_t pet_store_max_upload_size(void);
int pet_store_upload_begin(uint32_t pack_size);
int pet_store_upload_write(uint32_t offset, const uint8_t *data, size_t size);
int pet_store_upload_finish(uint32_t expected_crc32);
void pet_store_upload_abort(void);

uint32_t pet_crc32(const uint8_t *data, size_t size);
uint32_t pet_crc32_update(uint32_t crc, const uint8_t *data, size_t size);

