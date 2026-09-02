/* SPDX-License-Identifier: MIT */

#include "merry_link.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "merry_runtime.h"
#include "pinmap.h"

#define MERRY_LINK_MAGIC 0x314b4c4du /* MLK1 */
#define MERRY_LINK_VERSION 1u
#define MERRY_LINK_FLAG_PAYLOAD (1u << 0)
#define MERRY_LINK_FLAG_ACK (1u << 1)
#define MERRY_LINK_STREAM_SIZE 4096u

static const char *TAG = "merry-link";

struct merry_link_header {
    uint32_t magic;
    uint8_t version;
    uint8_t flags;
    uint8_t status;
    uint8_t reserved8;
    uint32_t session;
    uint16_t sequence;
    uint16_t ack_sequence;
    uint16_t payload_size;
    uint16_t reserved16;
    uint32_t payload_crc32;
    uint32_t header_crc32;
    uint32_t peer_session;
} __attribute__((packed));

struct merry_link_frame {
    struct merry_link_header header;
    uint8_t payload[MERRY_LINK_PAYLOAD_SIZE];
} __attribute__((packed));

_Static_assert(sizeof(struct merry_link_header) == 32, "link header changed");
_Static_assert(sizeof(struct merry_link_frame) == 544, "link frame changed");

static StreamBufferHandle_t incoming_stream;
static StreamBufferHandle_t outgoing_stream;
static uint8_t incoming_storage[MERRY_LINK_STREAM_SIZE + 1u];
static uint8_t outgoing_storage[MERRY_LINK_STREAM_SIZE + 1u];
static StaticStreamBuffer_t incoming_control;
static StaticStreamBuffer_t outgoing_control;
static DMA_ATTR struct merry_link_frame spi_tx_frame;
static DMA_ATTR struct merry_link_frame spi_rx_frame;

static uint32_t header_crc(const struct merry_link_header *header) {
    struct merry_link_header copy = *header;
    copy.header_crc32 = 0;
    return merry_crc32(&copy, sizeof(copy));
}

static bool frame_valid(const struct merry_link_frame *frame) {
    const struct merry_link_header *header = &frame->header;
    if (header->magic != MERRY_LINK_MAGIC || header->version != MERRY_LINK_VERSION ||
        header->reserved8 != 0 || header->reserved16 != 0 ||
        header->payload_size > MERRY_LINK_PAYLOAD_SIZE ||
        header_crc(header) != header->header_crc32) {
        return false;
    }
    if ((header->flags & MERRY_LINK_FLAG_PAYLOAD) == 0) {
        return header->payload_size == 0;
    }
    return header->payload_size != 0 &&
           merry_crc32(frame->payload, header->payload_size) == header->payload_crc32;
}

static void ready_set(bool ready) {
    gpio_set_level(MERRY_LINK_PIN_READY, ready ? 1 : 0);
}

static void link_task(void *unused) {
    (void)unused;

    const uint32_t local_session = esp_random() | 1u;
    uint32_t remote_session = 0;
    uint16_t tx_sequence = 0;
    uint16_t expected_rx_sequence = 0;
    uint16_t last_rx_sequence = 0;
    bool remote_known = false;
    bool have_rx_ack = false;
    bool outgoing_pending = false;
    size_t outgoing_size = 0;
    uint32_t transaction_count = 0;
    uint32_t invalid_count = 0;

    ESP_LOGI(TAG, "SPI3 slave ready: SCK=%d MOSI=%d MISO=%d CS=%d READY=%d frame=%u",
             MERRY_LINK_PIN_SCLK, MERRY_LINK_PIN_MOSI, MERRY_LINK_PIN_MISO,
             MERRY_LINK_PIN_CS, MERRY_LINK_PIN_READY,
             (unsigned)sizeof(struct merry_link_frame));

    while (true) {
        if (!outgoing_pending) {
            outgoing_size = xStreamBufferReceive(outgoing_stream, spi_tx_frame.payload,
                                                  MERRY_LINK_PAYLOAD_SIZE, 0);
            outgoing_pending = outgoing_size != 0;
        }

        memset(&spi_tx_frame.header, 0, sizeof(spi_tx_frame.header));
        spi_tx_frame.header.magic = MERRY_LINK_MAGIC;
        spi_tx_frame.header.version = MERRY_LINK_VERSION;
        spi_tx_frame.header.session = local_session;
        spi_tx_frame.header.peer_session = remote_known ? remote_session : 0;
        if (outgoing_pending) {
            spi_tx_frame.header.flags |= MERRY_LINK_FLAG_PAYLOAD;
            spi_tx_frame.header.sequence = tx_sequence;
            spi_tx_frame.header.payload_size = (uint16_t)outgoing_size;
            spi_tx_frame.header.payload_crc32 =
                merry_crc32(spi_tx_frame.payload, outgoing_size);
        }
        if (have_rx_ack) {
            spi_tx_frame.header.flags |= MERRY_LINK_FLAG_ACK;
            spi_tx_frame.header.ack_sequence = last_rx_sequence;
        }
        spi_tx_frame.header.header_crc32 = header_crc(&spi_tx_frame.header);

        spi_slave_transaction_t transaction = {
            .length = sizeof(spi_tx_frame) * 8u,
            .tx_buffer = &spi_tx_frame,
            .rx_buffer = &spi_rx_frame,
        };
        ESP_ERROR_CHECK(spi_slave_queue_trans(SPI3_HOST, &transaction, portMAX_DELAY));
        /* READY is a transaction-armed handshake, not a data-available flag.
         * The nRF must never assert CS until this DMA descriptor is queued. */
        ready_set(true);
        spi_slave_transaction_t *completed = NULL;
        ESP_ERROR_CHECK(spi_slave_get_trans_result(SPI3_HOST, &completed, portMAX_DELAY));
        ESP_ERROR_CHECK(completed == &transaction ? ESP_OK : ESP_ERR_INVALID_STATE);
        ready_set(false);

        ++transaction_count;
        if (transaction_count <= 4u || (transaction_count % 256u) == 0u) {
            ESP_LOGI(TAG, "SPI transaction %lu completed", (unsigned long)transaction_count);
        }

        if (!frame_valid(&spi_rx_frame)) {
            ++invalid_count;
            if (invalid_count <= 8u || (invalid_count % 256u) == 0u) {
                ESP_LOGW(TAG,
                         "invalid frame %lu: magic=%08lx version=%u flags=%02x size=%u "
                         "header_crc=%08lx expected=%08lx",
                         (unsigned long)invalid_count,
                         (unsigned long)spi_rx_frame.header.magic,
                         spi_rx_frame.header.version, spi_rx_frame.header.flags,
                         spi_rx_frame.header.payload_size,
                         (unsigned long)spi_rx_frame.header.header_crc32,
                         (unsigned long)header_crc(&spi_rx_frame.header));
            }
            continue;
        }
        const struct merry_link_header *received = &spi_rx_frame.header;
        if (transaction_count <= 8u) {
            ESP_LOGI(TAG,
                     "valid frame: session=%08lx peer=%08lx seq=%u ack=%u flags=%02x size=%u",
                     (unsigned long)received->session,
                     (unsigned long)received->peer_session, received->sequence,
                     received->ack_sequence, received->flags, received->payload_size);
        }
        const bool remote_changed = !remote_known || received->session != remote_session;
        if (remote_changed) {
            remote_session = received->session;
            remote_known = true;
            have_rx_ack = false;
            if ((received->flags & MERRY_LINK_FLAG_PAYLOAD) != 0) {
                expected_rx_sequence = received->sequence;
            }
            /* Responses queued for a previous nRF boot are no longer useful. */
            outgoing_pending = false;
            outgoing_size = 0;
        }

        /* A frame prepared for an earlier boot of this ESP is stale. A zero
         * peer session is allowed only for first-contact discovery. */
        if (received->peer_session != 0 && received->peer_session != local_session) {
            continue;
        }

        if (!remote_changed && outgoing_pending &&
            (received->flags & MERRY_LINK_FLAG_ACK) != 0 &&
            received->ack_sequence == tx_sequence) {
            outgoing_pending = false;
            outgoing_size = 0;
            ++tx_sequence;
        }

        if ((received->flags & MERRY_LINK_FLAG_PAYLOAD) == 0) {
            continue;
        }
        if (have_rx_ack && received->sequence == last_rx_sequence) {
            continue;
        }
        if (received->sequence != expected_rx_sequence) {
            continue;
        }
        if (xStreamBufferSend(incoming_stream, spi_rx_frame.payload,
                              received->payload_size, portMAX_DELAY) !=
            received->payload_size) {
            continue;
        }
        last_rx_sequence = received->sequence;
        expected_rx_sequence = (uint16_t)(received->sequence + 1u);
        have_rx_ack = true;
    }
}

esp_err_t merry_link_start(void) {
    incoming_stream = xStreamBufferCreateStatic(MERRY_LINK_STREAM_SIZE, 1,
                                                 incoming_storage, &incoming_control);
    outgoing_stream = xStreamBufferCreateStatic(MERRY_LINK_STREAM_SIZE, 1,
                                                 outgoing_storage, &outgoing_control);
    if (incoming_stream == NULL || outgoing_stream == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const gpio_config_t ready_config = {
        .pin_bit_mask = 1ULL << MERRY_LINK_PIN_READY,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&ready_config), "merry-link", "READY GPIO");
    ready_set(false);

    const spi_bus_config_t bus = {
        .mosi_io_num = MERRY_LINK_PIN_MOSI,
        .miso_io_num = MERRY_LINK_PIN_MISO,
        .sclk_io_num = MERRY_LINK_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = sizeof(struct merry_link_frame),
    };
    const spi_slave_interface_config_t slave = {
        .spics_io_num = MERRY_LINK_PIN_CS,
        .flags = 0,
        .queue_size = 1,
        .mode = 0,
    };
    ESP_RETURN_ON_ERROR(spi_slave_initialize(SPI3_HOST, &bus, &slave, SPI_DMA_CH_AUTO),
                        "merry-link", "SPI3 slave");
    if (xTaskCreatePinnedToCore(link_task, "merry-link", 4096, NULL, 10, NULL, 0) != pdPASS) {
        spi_slave_free(SPI3_HOST);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool merry_link_read(void *destination, size_t size, uint32_t timeout_ms) {
    uint8_t *bytes = destination;
    size_t offset = 0;
    const TickType_t timeout = timeout_ms == UINT32_MAX ? portMAX_DELAY
                                                        : pdMS_TO_TICKS(timeout_ms);
    const TickType_t started = xTaskGetTickCount();
    while (offset < size) {
        TickType_t wait = timeout;
        if (timeout != portMAX_DELAY) {
            const TickType_t elapsed = xTaskGetTickCount() - started;
            if (elapsed >= timeout) {
                return false;
            }
            wait = timeout - elapsed;
        }
        const size_t received =
            xStreamBufferReceive(incoming_stream, bytes + offset, size - offset, wait);
        if (received == 0) {
            return false;
        }
        offset += received;
    }
    return true;
}

bool merry_link_write(const void *source, size_t size, uint32_t timeout_ms) {
    const uint8_t *bytes = source;
    size_t offset = 0;
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    while (offset < size) {
        const size_t written =
            xStreamBufferSend(outgoing_stream, bytes + offset, size - offset, timeout);
        if (written == 0) {
            return false;
        }
        offset += written;
    }
    /* The link task owns READY. If an empty transaction is already armed, the
     * master's idle poll clocks it and the following queued frame carries this
     * response. This avoids claiming READY before a DMA descriptor exists. */
    return true;
}
