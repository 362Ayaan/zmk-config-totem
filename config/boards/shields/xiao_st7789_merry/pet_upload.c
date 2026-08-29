/* SPDX-License-Identifier: MIT */

#include "pet_store.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#define MERRY_UART_NODE DT_ALIAS(merry_uart)
#define UPLOAD_MAGIC 0x3150554du /* MUP1 */
#define CHUNK_MAGIC 0x3148434du  /* MCH1 */
#define RESPONSE_MAGIC 0x3153524du /* MRS1 */
#define UPLOAD_CHUNK_SIZE 512u
#define UPLOAD_READY_SEQUENCE 0xffffu
#define UPLOAD_FINAL_SEQUENCE 0xfffeu

enum upload_status {
    UPLOAD_OK = 0,
    UPLOAD_BAD_HEADER = 1,
    UPLOAD_BAD_SIZE = 2,
    UPLOAD_ERASE_FAILED = 3,
    UPLOAD_BAD_CHUNK = 4,
    UPLOAD_BAD_SEQUENCE = 5,
    UPLOAD_BAD_CRC = 6,
    UPLOAD_WRITE_FAILED = 7,
    UPLOAD_FINALIZE_FAILED = 8,
};

struct upload_header {
    uint32_t pack_size;
    uint32_t pack_crc32;
    uint32_t reserved;
} __packed;

struct chunk_header {
    uint16_t sequence;
    uint16_t size;
    uint32_t crc32;
} __packed;

BUILD_ASSERT(DT_NODE_HAS_STATUS(MERRY_UART_NODE, okay), "Merry CDC UART alias is not ready");
BUILD_ASSERT(sizeof(struct upload_header) == 12u, "upload header layout changed");
BUILD_ASSERT(sizeof(struct chunk_header) == 8u, "chunk header layout changed");

static const struct device *const upload_uart = DEVICE_DT_GET(MERRY_UART_NODE);

static int read_byte(uint8_t *value) {
    while (true) {
        int rc = uart_poll_in(upload_uart, value);
        if (rc == 0) {
            return 0;
        }
        if (rc != -1) {
            return rc;
        }
        k_sleep(K_MSEC(1));
    }
}

static int read_exact(void *destination, size_t size) {
    uint8_t *bytes = destination;
    for (size_t index = 0; index < size; index++) {
        int rc = read_byte(&bytes[index]);
        if (rc < 0) {
            return rc;
        }
    }
    return 0;
}

static int wait_for_magic(uint32_t expected) {
    uint32_t window = 0u;
    while (true) {
        uint8_t byte;
        int rc = read_byte(&byte);
        if (rc < 0) {
            return rc;
        }
        window = (window >> 8) | ((uint32_t)byte << 24);
        if (window == expected) {
            return 0;
        }
    }
}

static void send_response(uint16_t sequence, enum upload_status status) {
    uint8_t response[8];
    sys_put_le32(RESPONSE_MAGIC, &response[0]);
    sys_put_le16(sequence, &response[4]);
    sys_put_le16((uint16_t)status, &response[6]);
    for (size_t index = 0; index < sizeof(response); index++) {
        uart_poll_out(upload_uart, response[index]);
    }
}

static void upload_thread(void *unused1, void *unused2, void *unused3) {
    ARG_UNUSED(unused1);
    ARG_UNUSED(unused2);
    ARG_UNUSED(unused3);

    if (!device_is_ready(upload_uart)) {
        return;
    }

    uint8_t chunk[UPLOAD_CHUNK_SIZE];
    while (true) {
        if (wait_for_magic(UPLOAD_MAGIC) < 0) {
            k_sleep(K_MSEC(100));
            continue;
        }

        struct upload_header upload;
        if (read_exact(&upload, sizeof(upload)) < 0 || upload.reserved != 0u) {
            send_response(UPLOAD_READY_SEQUENCE, UPLOAD_BAD_HEADER);
            continue;
        }
        if (upload.pack_size < sizeof(struct pet_pack_header) ||
            upload.pack_size > pet_store_max_upload_size() || (upload.pack_size & 3u) != 0u) {
            send_response(UPLOAD_READY_SEQUENCE, UPLOAD_BAD_SIZE);
            continue;
        }

        int rc = pet_store_upload_begin(upload.pack_size);
        if (rc < 0) {
            send_response(UPLOAD_READY_SEQUENCE, UPLOAD_ERASE_FAILED);
            continue;
        }
        send_response(UPLOAD_READY_SEQUENCE, UPLOAD_OK);

        uint32_t offset = 0u;
        uint16_t expected_sequence = 0u;
        bool failed = false;
        while (offset < upload.pack_size) {
            if (wait_for_magic(CHUNK_MAGIC) < 0) {
                failed = true;
                break;
            }

            struct chunk_header header;
            if (read_exact(&header, sizeof(header)) < 0 || header.size == 0u ||
                header.size > sizeof(chunk) || header.size > upload.pack_size - offset) {
                send_response(expected_sequence, UPLOAD_BAD_CHUNK);
                failed = true;
                break;
            }
            if (read_exact(chunk, header.size) < 0) {
                send_response(expected_sequence, UPLOAD_BAD_CHUNK);
                failed = true;
                break;
            }
            if (header.sequence != expected_sequence) {
                send_response(header.sequence, UPLOAD_BAD_SEQUENCE);
                failed = true;
                break;
            }
            if (pet_crc32(chunk, header.size) != header.crc32) {
                send_response(header.sequence, UPLOAD_BAD_CRC);
                failed = true;
                break;
            }

            rc = pet_store_upload_write(offset, chunk, header.size);
            if (rc < 0) {
                send_response(header.sequence, UPLOAD_WRITE_FAILED);
                failed = true;
                break;
            }
            send_response(header.sequence, UPLOAD_OK);
            offset += header.size;
            expected_sequence++;
        }

        if (failed) {
            pet_store_upload_abort();
            continue;
        }

        rc = pet_store_upload_finish(upload.pack_crc32);
        send_response(UPLOAD_FINAL_SEQUENCE, rc == 0 ? UPLOAD_OK : UPLOAD_FINALIZE_FAILED);
    }
}

K_THREAD_DEFINE(merry_upload_thread, 2048, upload_thread, NULL, NULL, NULL, 12, 0, 0);
