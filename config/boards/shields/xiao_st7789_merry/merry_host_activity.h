/* SPDX-License-Identifier: MIT */

#pragma once

#include <stdint.h>

enum merry_host_state {
    MERRY_HOST_AFK = 0,
    MERRY_HOST_ACTIVE = 1,
    MERRY_HOST_DISPLAY_OFF = 2,
};

uint8_t merry_host_state_get(void);
int merry_host_state_set(uint8_t state);
