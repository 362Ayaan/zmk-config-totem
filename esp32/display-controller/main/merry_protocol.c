/* SPDX-License-Identifier: MIT */

#include "merry_protocol.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "merry_runtime.h"

#define CODEX_STATE_MAGIC 0x3153434du          /* MCS1 */
#define CODEX_RESPONSE_MAGIC 0x3141434du       /* MCA1 */
#define MEDIA_UPLOAD_MAGIC 0x3155414du         /* MAU1 */
#define MEDIA_CHUNK_MAGIC 0x3143414du          /* MAC1 */
#define MEDIA_RESPONSE_MAGIC 0x3152414du       /* MAR1 */
#define MEDIA_STATE_MAGIC 0x31534d4du          /* MMS1 */
#define MEDIA_STATE_RESPONSE_MAGIC 0x31414d4du /* MMA1 */
#define HOST_STATE_MAGIC 0x3153484du           /* MHS1 */
#define HOST_STATE_RESPONSE_MAGIC 0x3141484du  /* MHA1 */

#define PROTOCOL_VERSION 1u
#define MIN_TTL_MS 5000u
#define MAX_TTL_MS 60000u
#define CHUNK_SIZE 512u
#define READY_SEQUENCE 0xffffu
#define FINAL_SEQUENCE 0xfffeu
#define PACKET_TIMEOUT_MS 2000u

enum protocol_status {
    STATUS_OK = 0,
    STATUS_BAD_HEADER = 1,
    STATUS_BAD_SIZE = 2,
    STATUS_BAD_CHUNK = 3,
    STATUS_BAD_SEQUENCE = 4,
    STATUS_BAD_CRC = 5,
    STATUS_WRITE_FAILED = 6,
    STATUS_FINALIZE_FAILED = 7,
    STATUS_BAD_STATE = 8,
};

struct state_request {
    uint8_t version;
    uint8_t state;
    uint16_t sequence;
    uint32_t ttl_ms;
    uint32_t crc32;
} __attribute__((packed));

struct state_response {
    uint32_t magic;
    uint8_t status;
    uint8_t state;
    uint16_t sequence;
} __attribute__((packed));

struct media_upload_header {
    uint8_t version;
    uint8_t state;
    uint16_t width;
    uint16_t height;
    uint16_t reserved;
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t generation;
} __attribute__((packed));

struct chunk_header {
    uint16_t sequence;
    uint16_t size;
    uint32_t crc32;
} __attribute__((packed));

struct media_response {
    uint32_t magic;
    uint16_t sequence;
    uint16_t status;
} __attribute__((packed));

_Static_assert(sizeof(struct state_request) == 12, "state request changed");
_Static_assert(sizeof(struct state_response) == 8, "state response changed");
_Static_assert(sizeof(struct media_upload_header) == 20, "media header changed");
_Static_assert(sizeof(struct chunk_header) == 8, "chunk header changed");
_Static_assert(sizeof(struct media_response) == 8, "media response changed");

static uint32_t pending_magic;

static bool is_command(uint32_t magic) {
    return magic == CODEX_STATE_MAGIC || magic == MEDIA_UPLOAD_MAGIC ||
           magic == MEDIA_STATE_MAGIC || magic == HOST_STATE_MAGIC;
}

static bool read_exact(void *destination, size_t size, uint32_t timeout_ms) {
    uint8_t *bytes = destination;
    size_t offset = 0;
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (offset < size) {
        const int64_t remaining_us = deadline - esp_timer_get_time();
        if (remaining_us <= 0) {
            return false;
        }
        TickType_t wait = pdMS_TO_TICKS((remaining_us + 999) / 1000);
        if (wait == 0) {
            wait = 1;
        }
        const int received = usb_serial_jtag_read_bytes(bytes + offset, size - offset, wait);
        if (received < 0) {
            return false;
        }
        offset += (size_t)received;
    }
    return true;
}

static bool write_exact(const void *source, size_t size) {
    const uint8_t *bytes = source;
    size_t offset = 0;
    while (offset < size) {
        const int written = usb_serial_jtag_write_bytes(bytes + offset, size - offset,
                                                        pdMS_TO_TICKS(PACKET_TIMEOUT_MS));
        if (written <= 0) {
            return false;
        }
        offset += (size_t)written;
    }
    return true;
}

/* Finds an expected framed marker while also recognizing a fresh command. A
 * new command interrupts an abandoned media upload immediately instead of
 * leaving the parser trapped waiting for another media chunk. */
static bool wait_for_magic(uint32_t expected, uint32_t timeout_ms, bool interruptible) {
    uint32_t window = 0;
    const int64_t deadline = timeout_ms == UINT32_MAX
                                 ? INT64_MAX
                                 : esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        uint8_t byte;
        if (!read_exact(&byte, 1, timeout_ms == UINT32_MAX ? 100 : 50)) {
            continue;
        }
        window = (window >> 8) | ((uint32_t)byte << 24);
        if (window == expected) {
            return true;
        }
        if (interruptible && is_command(window)) {
            pending_magic = window;
            return false;
        }
    }
    return false;
}

static uint32_t wait_for_command(void) {
    if (pending_magic != 0) {
        const uint32_t command = pending_magic;
        pending_magic = 0;
        return command;
    }
    uint32_t window = 0;
    while (true) {
        uint8_t byte;
        if (!read_exact(&byte, 1, 100)) {
            continue;
        }
        window = (window >> 8) | ((uint32_t)byte << 24);
        if (is_command(window)) {
            return window;
        }
    }
}

static void send_state_response(uint32_t magic, uint8_t status, uint8_t state,
                                uint16_t sequence) {
    const struct state_response response = {
        .magic = magic,
        .status = status,
        .state = state,
        .sequence = sequence,
    };
    (void)write_exact(&response, sizeof(response));
}

static void handle_codex_state(void) {
    struct state_request request = {0};
    if (!read_exact(&request, sizeof(request), PACKET_TIMEOUT_MS) ||
        request.version != PROTOCOL_VERSION || request.state > MERRY_ANIM_BLOCKED ||
        request.ttl_ms < MIN_TTL_MS || request.ttl_ms > MAX_TTL_MS) {
        send_state_response(CODEX_RESPONSE_MAGIC, STATUS_BAD_HEADER,
                            merry_runtime_codex_state(), request.sequence);
        return;
    }
    if (merry_crc32(&request, offsetof(struct state_request, crc32)) != request.crc32) {
        send_state_response(CODEX_RESPONSE_MAGIC, 2,
                            merry_runtime_codex_state(), request.sequence);
        return;
    }
    const bool accepted = merry_runtime_set_codex(request.state, request.ttl_ms);
    send_state_response(CODEX_RESPONSE_MAGIC, accepted ? STATUS_OK : 3,
                        merry_runtime_codex_state(), request.sequence);
}

static void handle_media_state(void) {
    struct state_request request = {0};
    if (!read_exact(&request, sizeof(request), PACKET_TIMEOUT_MS) ||
        request.version != PROTOCOL_VERSION || request.state > MERRY_MEDIA_PAUSED ||
        request.ttl_ms < MIN_TTL_MS || request.ttl_ms > MAX_TTL_MS) {
        send_state_response(MEDIA_STATE_RESPONSE_MAGIC, STATUS_BAD_HEADER,
                            merry_runtime_media_state(), request.sequence);
        return;
    }
    if (merry_crc32(&request, offsetof(struct state_request, crc32)) != request.crc32) {
        send_state_response(MEDIA_STATE_RESPONSE_MAGIC, STATUS_BAD_CRC,
                            merry_runtime_media_state(), request.sequence);
        return;
    }
    const bool accepted = merry_runtime_set_media(request.state, request.ttl_ms);
    send_state_response(MEDIA_STATE_RESPONSE_MAGIC, accepted ? STATUS_OK : STATUS_BAD_STATE,
                        merry_runtime_media_state(), request.sequence);
}

static void handle_host_state(void) {
    struct state_request request = {0};
    if (!read_exact(&request, sizeof(request), PACKET_TIMEOUT_MS) ||
        request.version != PROTOCOL_VERSION || request.state > MERRY_HOST_DISPLAY_OFF ||
        request.ttl_ms < MIN_TTL_MS || request.ttl_ms > MAX_TTL_MS) {
        send_state_response(HOST_STATE_RESPONSE_MAGIC, STATUS_BAD_HEADER,
                            merry_runtime_host_state(), request.sequence);
        return;
    }
    if (merry_crc32(&request, offsetof(struct state_request, crc32)) != request.crc32) {
        send_state_response(HOST_STATE_RESPONSE_MAGIC, 2,
                            merry_runtime_host_state(), request.sequence);
        return;
    }
    const bool accepted = merry_runtime_set_host(request.state, request.ttl_ms);
    send_state_response(HOST_STATE_RESPONSE_MAGIC, accepted ? STATUS_OK : STATUS_BAD_STATE,
                        merry_runtime_host_state(), request.sequence);
}

static void send_media_response(uint16_t sequence, uint16_t status) {
    const struct media_response response = {
        .magic = MEDIA_RESPONSE_MAGIC,
        .sequence = sequence,
        .status = status,
    };
    (void)write_exact(&response, sizeof(response));
}

static void handle_media_upload(void) {
    struct media_upload_header upload = {0};
    if (!read_exact(&upload, sizeof(upload), PACKET_TIMEOUT_MS) ||
        upload.version != PROTOCOL_VERSION || upload.state < MERRY_MEDIA_PLAYING ||
        upload.state > MERRY_MEDIA_PAUSED || upload.width != MERRY_MEDIA_WIDTH ||
        upload.height != MERRY_MEDIA_HEIGHT || upload.reserved != 0 ||
        upload.image_size != MERRY_MEDIA_BYTES) {
        send_media_response(READY_SEQUENCE, STATUS_BAD_HEADER);
        return;
    }
    if (!merry_runtime_media_upload_begin()) {
        send_media_response(READY_SEQUENCE, STATUS_WRITE_FAILED);
        return;
    }
    send_media_response(READY_SEQUENCE, STATUS_OK);

    uint8_t chunk[CHUNK_SIZE];
    uint32_t offset = 0;
    uint16_t expected_sequence = 0;
    while (offset < upload.image_size) {
        if (!wait_for_magic(MEDIA_CHUNK_MAGIC, PACKET_TIMEOUT_MS, true)) {
            merry_runtime_media_upload_abort();
            return;
        }
        struct chunk_header header = {0};
        if (!read_exact(&header, sizeof(header), PACKET_TIMEOUT_MS) || header.size == 0 ||
            header.size > sizeof(chunk) || header.size > upload.image_size - offset) {
            send_media_response(expected_sequence, STATUS_BAD_CHUNK);
            merry_runtime_media_upload_abort();
            return;
        }
        if (!read_exact(chunk, header.size, PACKET_TIMEOUT_MS)) {
            send_media_response(expected_sequence, STATUS_BAD_CHUNK);
            merry_runtime_media_upload_abort();
            return;
        }
        if (header.sequence != expected_sequence) {
            send_media_response(header.sequence, STATUS_BAD_SEQUENCE);
            merry_runtime_media_upload_abort();
            return;
        }
        if (merry_crc32(chunk, header.size) != header.crc32) {
            send_media_response(header.sequence, STATUS_BAD_CRC);
            merry_runtime_media_upload_abort();
            return;
        }
        if (!merry_runtime_media_upload_write(offset, chunk, header.size)) {
            send_media_response(header.sequence, STATUS_WRITE_FAILED);
            merry_runtime_media_upload_abort();
            return;
        }
        send_media_response(header.sequence, STATUS_OK);
        offset += header.size;
        ++expected_sequence;
    }

    const bool accepted = merry_runtime_media_upload_finish(upload.image_crc32, upload.state);
    send_media_response(FINAL_SEQUENCE, accepted ? STATUS_OK : STATUS_FINALIZE_FAILED);
}

static void protocol_task(void *unused) {
    (void)unused;
    while (true) {
        const uint32_t command = wait_for_command();
        if (command == CODEX_STATE_MAGIC) {
            handle_codex_state();
        } else if (command == MEDIA_STATE_MAGIC) {
            handle_media_state();
        } else if (command == HOST_STATE_MAGIC) {
            handle_host_state();
        } else if (command == MEDIA_UPLOAD_MAGIC) {
            handle_media_upload();
        }
    }
}

esp_err_t merry_protocol_start(void) {
    const usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = 2048,
        .rx_buffer_size = 4096,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&config);
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreatePinnedToCore(protocol_task, "merry-protocol", 6144, NULL, 8, NULL, 0) !=
        pdPASS) {
        usb_serial_jtag_driver_uninstall();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
