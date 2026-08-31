/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/toolchain.h>

#define MERRY_CONFIG_VERSION 3u
#define MERRY_CONFIG_DEFAULT_TIMEOUT_MS 20000u
#define MERRY_CONFIG_MIN_TIMEOUT_MS 1000u
#define MERRY_CONFIG_MAX_TIMEOUT_MS 3600000u
#define MERRY_CONFIG_MIN_X (-40)
#define MERRY_CONFIG_MAX_X 40
#define MERRY_CONFIG_MIN_Y (-33)
#define MERRY_CONFIG_MAX_Y 33

enum merry_display_mode {
    MERRY_DISPLAY_AUTO = 0,
    MERRY_DISPLAY_DASHBOARD = 1,
    MERRY_DISPLAY_PET = 2,
    MERRY_DISPLAY_OFF = 3,
};

struct merry_config {
    uint8_t version;
    uint8_t display_mode;
    uint8_t animation_id;
    uint8_t brightness;
    uint32_t idle_timeout_ms;
    int16_t pet_x;
    int16_t pet_y;
    uint8_t battery_left_slot;
    uint8_t battery_dial_slot;
    uint8_t battery_right_slot;
    uint8_t reserved;
} __packed;

int merry_config_get(struct merry_config *config);
int merry_config_set(const struct merry_config *config);
int merry_config_reset(void);
bool merry_config_is_valid(const struct merry_config *config);

/* Implemented by the status screen. Safe to call before LVGL initializes. */
void merry_screen_config_changed(void);
