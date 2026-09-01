/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define MERRY_LINK_PAYLOAD_SIZE 512u

esp_err_t merry_link_start(void);
bool merry_link_read(void *destination, size_t size, uint32_t timeout_ms);
bool merry_link_write(const void *source, size_t size, uint32_t timeout_ms);
