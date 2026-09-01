/* SPDX-License-Identifier: MIT */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>

#define MERRY_UART_NODE DT_ALIAS(merry_uart)
#define MERRY_LINK_CONFIG DT_NODELABEL(merry_link_config)
#define MERRY_LINK_SPI DT_NODELABEL(xiao_spi)

#define MERRY_LINK_MAGIC 0x314b4c4du /* MLK1 */
#define MERRY_LINK_VERSION 1u
#define MERRY_LINK_FLAG_PAYLOAD BIT(0)
#define MERRY_LINK_FLAG_ACK BIT(1)
#define MERRY_LINK_PAYLOAD_SIZE 512u
#define MERRY_LINK_SPI_HZ 8000000u

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
} __packed;

struct merry_link_frame {
    struct merry_link_header header;
    uint8_t payload[MERRY_LINK_PAYLOAD_SIZE];
} __packed;

BUILD_ASSERT(sizeof(struct merry_link_header) == 32, "link header changed");
BUILD_ASSERT(sizeof(struct merry_link_frame) == 544, "link frame changed");

static const struct device *const upload_uart = DEVICE_DT_GET(MERRY_UART_NODE);
static const struct device *const link_spi = DEVICE_DT_GET(MERRY_LINK_SPI);
static const struct gpio_dt_spec ready_gpio =
    GPIO_DT_SPEC_GET(MERRY_LINK_CONFIG, ready_gpios);
static struct spi_cs_control link_cs = {
    .gpio = GPIO_DT_SPEC_GET(MERRY_LINK_CONFIG, cs_gpios),
    .delay = 0,
};
static const struct spi_config link_spi_config = {
    .frequency = MERRY_LINK_SPI_HZ,
    .operation = SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8),
    .slave = 0,
    .cs = &link_cs,
};

static struct merry_link_frame spi_tx_frame __aligned(4);
static struct merry_link_frame spi_rx_frame __aligned(4);

static uint32_t crc32_bytes(const void *data, size_t size) {
    const uint8_t *bytes = data;
    uint32_t crc = 0xffffffffu;
    for (size_t index = 0; index < size; ++index) {
        crc ^= bytes[index];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return ~crc;
}

static uint32_t header_crc(const struct merry_link_header *header) {
    struct merry_link_header copy = *header;
    copy.header_crc32 = 0;
    return crc32_bytes(&copy, sizeof(copy));
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
           crc32_bytes(frame->payload, header->payload_size) == header->payload_crc32;
}

static int link_exchange(void) {
    const struct spi_buf tx_buffer = {.buf = &spi_tx_frame, .len = sizeof(spi_tx_frame)};
    const struct spi_buf rx_buffer = {.buf = &spi_rx_frame, .len = sizeof(spi_rx_frame)};
    const struct spi_buf_set tx = {.buffers = &tx_buffer, .count = 1};
    const struct spi_buf_set rx = {.buffers = &rx_buffer, .count = 1};
    return spi_transceive(link_spi, &link_spi_config, &tx, &rx);
}

static void usb_write(const uint8_t *bytes, size_t size) {
    for (size_t index = 0; index < size; ++index) {
        uart_poll_out(upload_uart, bytes[index]);
    }
}

static void bridge_thread(void *unused1, void *unused2, void *unused3) {
    ARG_UNUSED(unused1);
    ARG_UNUSED(unused2);
    ARG_UNUSED(unused3);

    if (!device_is_ready(upload_uart) || !device_is_ready(link_spi) ||
        !gpio_is_ready_dt(&ready_gpio) || !gpio_is_ready_dt(&link_cs.gpio) ||
        gpio_pin_configure_dt(&ready_gpio, GPIO_INPUT) < 0) {
        return;
    }

    const uint32_t local_session = sys_rand32_get() | 1u;
    uint32_t remote_session = 0;
    uint16_t tx_sequence = 0;
    uint16_t expected_rx_sequence = 0;
    uint16_t last_rx_sequence = 0;
    bool remote_known = false;
    bool have_rx_ack = false;
    bool ack_pending = false;
    bool outgoing_pending = false;
    size_t outgoing_size = 0;

    while (true) {
        if (!outgoing_pending) {
            uint8_t byte;
            while (outgoing_size < MERRY_LINK_PAYLOAD_SIZE &&
                   uart_poll_in(upload_uart, &byte) == 0) {
                spi_tx_frame.payload[outgoing_size++] = byte;
            }
            outgoing_pending = outgoing_size != 0;
        }

        const bool esp_requests_clock = gpio_pin_get_dt(&ready_gpio) > 0;
        if (!outgoing_pending && !ack_pending && !esp_requests_clock) {
            k_sleep(K_MSEC(1));
            continue;
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
                crc32_bytes(spi_tx_frame.payload, outgoing_size);
        }
        if (have_rx_ack) {
            spi_tx_frame.header.flags |= MERRY_LINK_FLAG_ACK;
            spi_tx_frame.header.ack_sequence = last_rx_sequence;
        }
        spi_tx_frame.header.header_crc32 = header_crc(&spi_tx_frame.header);

        if (link_exchange() < 0) {
            k_sleep(K_MSEC(2));
            continue;
        }
        ack_pending = false;

        if (!frame_valid(&spi_rx_frame)) {
            continue;
        }
        const struct merry_link_header *received = &spi_rx_frame.header;
        const bool remote_changed = !remote_known || received->session != remote_session;
        if (remote_changed) {
            remote_session = received->session;
            remote_known = true;
            have_rx_ack = false;
            if ((received->flags & MERRY_LINK_FLAG_PAYLOAD) != 0) {
                expected_rx_sequence = received->sequence;
            }
        }
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
            ack_pending = true;
            continue;
        }
        if (received->sequence != expected_rx_sequence) {
            ack_pending = have_rx_ack;
            continue;
        }
        usb_write(spi_rx_frame.payload, received->payload_size);
        last_rx_sequence = received->sequence;
        expected_rx_sequence = (uint16_t)(received->sequence + 1u);
        have_rx_ack = true;
        ack_pending = true;
    }
}

K_THREAD_DEFINE(merry_spi_bridge_thread, 3072, bridge_thread, NULL, NULL, NULL, 12, 0, 0);
