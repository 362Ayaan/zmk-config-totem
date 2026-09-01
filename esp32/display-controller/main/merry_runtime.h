/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MERRY_LCD_WIDTH 240u
#define MERRY_LCD_HEIGHT 240u
#define MERRY_LCD_FRAME_BYTES (MERRY_LCD_WIDTH * MERRY_LCD_HEIGHT * sizeof(uint16_t))

#define MERRY_MEDIA_WIDTH 202u
#define MERRY_MEDIA_HEIGHT 220u
#define MERRY_MEDIA_BYTES (MERRY_MEDIA_WIDTH * MERRY_MEDIA_HEIGHT * sizeof(uint16_t))

enum merry_animation_id {
    MERRY_ANIM_IDLE = 0,
    MERRY_ANIM_RUNNING = 1,
    MERRY_ANIM_NEEDS_INPUT = 2,
    MERRY_ANIM_COMPLETED = 3,
    MERRY_ANIM_BLOCKED = 4,
};

enum merry_media_state {
    MERRY_MEDIA_NONE = 0,
    MERRY_MEDIA_PLAYING = 1,
    MERRY_MEDIA_PAUSED = 2,
};

enum merry_host_state {
    MERRY_HOST_AFK = 0,
    MERRY_HOST_ACTIVE = 1,
    MERRY_HOST_DISPLAY_OFF = 2,
};

esp_err_t merry_runtime_init(void);
void merry_runtime_attach_renderer(TaskHandle_t renderer);

/* Returns the number of milliseconds until the next animation deadline. */
uint32_t merry_runtime_render(uint16_t *destination, bool *changed, bool *screen_on);

uint8_t merry_runtime_codex_state(void);
bool merry_runtime_set_codex(uint8_t state, uint32_t ttl_ms);
uint8_t merry_runtime_media_state(void);
bool merry_runtime_set_media(uint8_t state, uint32_t ttl_ms);
uint8_t merry_runtime_host_state(void);
bool merry_runtime_set_host(uint8_t state, uint32_t ttl_ms);

bool merry_runtime_media_upload_begin(void);
bool merry_runtime_media_upload_write(size_t offset, const uint8_t *data, size_t size);
bool merry_runtime_media_upload_finish(uint32_t expected_crc, uint8_t state);
void merry_runtime_media_upload_abort(void);

uint32_t merry_crc32(const void *data, size_t size);
