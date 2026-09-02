/* SPDX-License-Identifier: MIT */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/split/central.h>

#define MERRY_UART_NODE DT_ALIAS(merry_uart)
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
} __packed;

BUILD_ASSERT(sizeof(struct keyboard_packet) == 32, "keyboard packet changed");

static const struct device *const status_uart = DEVICE_DT_GET(MERRY_UART_NODE);
static atomic_t activity_counter = ATOMIC_INIT(1);
/* Known-good historical pairing order is the fallback. Physical key events
 * teach these source IDs automatically after one press on each keyboard half. */
static atomic_t left_source = ATOMIC_INIT(2);
static atomic_t right_source = ATOMIC_INIT(1);
K_SEM_DEFINE(status_update, 1, 1);

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

static void send_packet(uint16_t sequence) {
    struct keyboard_packet packet = {
        .magic = KEYBOARD_MAGIC,
        .version = KEYBOARD_VERSION,
        .size = sizeof(struct keyboard_packet),
        .sequence = sequence,
        .activity_counter = (uint32_t)atomic_get(&activity_counter),
        .modifiers = zmk_hid_get_explicit_mods(),
    };
    zmk_keymap_layer_index_t layer_index = zmk_keymap_highest_layer_active();
    packet.layer = (uint8_t)layer_index;
    const char *name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(layer_index));
    snprintf(packet.layer_name, sizeof(packet.layer_name), "%s",
             name != NULL && name[0] != '\0' ? name : "BASE");
    const uint8_t left = (uint8_t)atomic_get(&left_source);
    const uint8_t right = (uint8_t)atomic_get(&right_source);
    uint8_t dial = 0;
    if (left < 3u && right < 3u && left != right) {
        dial = (uint8_t)(3u - left - right);
    }
    (void)zmk_split_central_get_peripheral_battery_level(left, &packet.battery_left);
    (void)zmk_split_central_get_peripheral_battery_level(dial, &packet.battery_dial);
    (void)zmk_split_central_get_peripheral_battery_level(right, &packet.battery_right);
    packet.crc32 = crc32_bytes(&packet, offsetof(struct keyboard_packet, crc32));

    const uint8_t *bytes = (const uint8_t *)&packet;
    for (size_t index = 0; index < sizeof(packet); ++index) {
        uart_poll_out(status_uart, bytes[index]);
    }
}

static void status_thread(void *unused1, void *unused2, void *unused3) {
    ARG_UNUSED(unused1);
    ARG_UNUSED(unused2);
    ARG_UNUSED(unused3);
    if (!device_is_ready(status_uart)) {
        return;
    }
    uint16_t sequence = 0;
    while (true) {
        const int updated = k_sem_take(&status_update, K_SECONDS(1));
        if (updated == 0) {
            /* Let a key/layer event finish updating ZMK's derived state before
             * taking the atomic snapshot. This thread never blocks USB HID. */
            k_sleep(K_MSEC(2));
        }
        send_packet(sequence++);
    }
}

static int keyboard_activity_listener(const zmk_event_t *event) {
    ARG_UNUSED(event);
    atomic_inc(&activity_counter);
    k_sem_give(&status_update);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(merry_uart_activity, keyboard_activity_listener);
ZMK_SUBSCRIPTION(merry_uart_activity, zmk_keycode_state_changed);
ZMK_SUBSCRIPTION(merry_uart_activity, zmk_layer_state_changed);

static bool position_is_left(uint32_t position) {
    return position <= 4u || (position >= 10u && position <= 14u) ||
           (position >= 20u && position <= 25u) ||
           (position >= 32u && position <= 34u);
}

static bool position_is_right(uint32_t position) {
    return (position >= 5u && position <= 9u) ||
           (position >= 15u && position <= 19u) ||
           (position >= 26u && position <= 31u) ||
           (position >= 35u && position <= 37u);
}

static int source_learning_listener(const zmk_event_t *event) {
    const struct zmk_position_state_changed *position =
        as_zmk_position_state_changed(event);
    if (position == NULL || !position->state || position->source >= 3u) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (position_is_left(position->position)) {
        atomic_set(&left_source, position->source);
    } else if (position_is_right(position->position)) {
        atomic_set(&right_source, position->source);
    }
    atomic_inc(&activity_counter);
    k_sem_give(&status_update);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(merry_uart_source_learning, source_learning_listener);
ZMK_SUBSCRIPTION(merry_uart_source_learning, zmk_position_state_changed);

static int battery_listener(const zmk_event_t *event) {
    ARG_UNUSED(event);
    k_sem_give(&status_update);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(merry_uart_battery, battery_listener);
ZMK_SUBSCRIPTION(merry_uart_battery, zmk_peripheral_battery_state_changed);

K_THREAD_DEFINE(merry_uart_status_thread, 2048, status_thread, NULL, NULL, NULL,
                14, 0, 0);
