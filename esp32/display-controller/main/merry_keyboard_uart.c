/* SPDX-License-Identifier: MIT */

#include "merry_keyboard_uart.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "merry_runtime.h"
#include "pinmap.h"

#define KEYBOARD_UART UART_NUM_1
#define KEYBOARD_BAUD 115200
#define KEYBOARD_MAGIC 0x31554b4du /* "MKU1" in little endian */
#define KEYBOARD_VERSION 1u

struct keyboard_packet {
    uint32_t magic;
    uint8_t version;
    uint8_t size;
    uint16_t sequence;
    uint32_t activity_counter;
    uint8_t layer;
    uint8_t modifiers;
    uint8_t battery_left;
    uint8_t battery_dial;
    uint8_t battery_right;
    uint8_t flags;
    char layer_name[10];
    uint32_t crc32;
} __attribute__((packed));

_Static_assert(sizeof(struct keyboard_packet) == 32, "keyboard packet changed");

static void keyboard_uart_task(void *unused) {
    (void)unused;
    struct keyboard_packet packet;
    uint32_t window = 0;
    while (true) {
        uint8_t byte;
        if (uart_read_bytes(KEYBOARD_UART, &byte, 1, pdMS_TO_TICKS(100)) != 1) {
            continue;
        }
        window = (window >> 8) | ((uint32_t)byte << 24);
        if (window != KEYBOARD_MAGIC) {
            continue;
        }
        packet.magic = window;
        uint8_t *tail = (uint8_t *)&packet + sizeof(packet.magic);
        const size_t tail_size = sizeof(packet) - sizeof(packet.magic);
        size_t offset = 0;
        while (offset < tail_size) {
            const int received = uart_read_bytes(KEYBOARD_UART, tail + offset,
                                                 tail_size - offset,
                                                 pdMS_TO_TICKS(20));
            if (received <= 0) {
                break;
            }
            offset += (size_t)received;
        }
        if (offset != tail_size || packet.version != KEYBOARD_VERSION ||
            packet.size != sizeof(packet) || packet.flags != 0 ||
            packet.crc32 != merry_crc32(&packet, offsetof(struct keyboard_packet, crc32))) {
            window = 0;
            continue;
        }
        packet.layer_name[sizeof(packet.layer_name) - 1u] = '\0';
        merry_runtime_set_keyboard(packet.activity_counter, packet.layer,
                                   packet.modifiers, packet.battery_left,
                                   packet.battery_dial, packet.battery_right,
                                   packet.layer_name);
        window = 0;
    }
}

esp_err_t merry_keyboard_uart_start(void) {
    const uart_config_t config = {
        .baud_rate = KEYBOARD_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(KEYBOARD_UART, 512, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    if ((err = uart_param_config(KEYBOARD_UART, &config)) != ESP_OK ||
        (err = uart_set_pin(KEYBOARD_UART, MERRY_KEYBOARD_UART_TX,
                            MERRY_KEYBOARD_UART_RX, UART_PIN_NO_CHANGE,
                            UART_PIN_NO_CHANGE)) != ESP_OK) {
        uart_driver_delete(KEYBOARD_UART);
        return err;
    }
    if (xTaskCreatePinnedToCore(keyboard_uart_task, "keyboard-uart", 3072, NULL, 9,
                                NULL, 0) != pdPASS) {
        uart_driver_delete(KEYBOARD_UART);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
