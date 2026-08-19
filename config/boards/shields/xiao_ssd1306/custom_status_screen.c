/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <string.h>

#include <lvgl.h>
#include <zephyr/kernel.h>

#include <zmk/ble.h>
#include <zmk/display.h>
#include <zmk/display/status_screen.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/split/central.h>

static lv_obj_t *left_battery_label;
static lv_obj_t *output_label;
static lv_obj_t *right_battery_label;
static lv_obj_t *layer_label;

struct layer_status_state {
    const char *name;
};

static struct layer_status_state layer_status_get_state(const zmk_event_t *eh) {
    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    const char *name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index));

    return (struct layer_status_state){.name = (name != NULL && name[0] != '\0') ? name : "BASE"};
}

static void layer_status_update(struct layer_status_state state) {
    lv_label_set_text(layer_label, state.name);
    lv_obj_align(layer_label, LV_ALIGN_CENTER, 0, 8);
}

ZMK_DISPLAY_WIDGET_LISTENER(clean_layer_status, struct layer_status_state, layer_status_update,
                            layer_status_get_state)
ZMK_SUBSCRIPTION(clean_layer_status, zmk_layer_state_changed);

struct peripheral_battery_state {
    uint8_t level[2];
};

static struct peripheral_battery_state peripheral_battery_get_state(const zmk_event_t *eh) {
    struct peripheral_battery_state state = {};

    zmk_split_central_get_peripheral_battery_level(0, &state.level[0]);
    zmk_split_central_get_peripheral_battery_level(1, &state.level[1]);
    return state;
}

static void set_battery_text(lv_obj_t *label, char side, uint8_t level) {
    char text[8];

    if (level == 0) {
        snprintf(text, sizeof(text), "%c:--", side);
    } else {
        snprintf(text, sizeof(text), "%c:%u%%", side, level);
    }
    lv_label_set_text(label, text);
}

static void peripheral_battery_update(struct peripheral_battery_state state) {
    set_battery_text(left_battery_label, 'L', state.level[0]);
    set_battery_text(right_battery_label, 'R', state.level[1]);
}

ZMK_DISPLAY_WIDGET_LISTENER(clean_battery_status, struct peripheral_battery_state,
                            peripheral_battery_update, peripheral_battery_get_state)
ZMK_SUBSCRIPTION(clean_battery_status, zmk_peripheral_battery_state_changed);

struct output_status_state {
    enum zmk_transport transport;
    uint8_t profile;
};

static struct output_status_state output_status_get_state(const zmk_event_t *eh) {
    struct zmk_endpoint_instance endpoint = zmk_endpoint_get_selected();

    return (struct output_status_state){
        .transport = endpoint.transport,
        .profile = endpoint.transport == ZMK_TRANSPORT_BLE ? endpoint.ble.profile_index + 1 : 0,
    };
}

static void output_status_update(struct output_status_state state) {
    char text[6];

    switch (state.transport) {
    case ZMK_TRANSPORT_USB:
        strcpy(text, "USB");
        break;
    case ZMK_TRANSPORT_BLE:
        snprintf(text, sizeof(text), "BT%u", state.profile);
        break;
    default:
        strcpy(text, "OFF");
        break;
    }

    lv_label_set_text(output_label, text);
    lv_obj_align(output_label, LV_ALIGN_TOP_MID, 0, 3);
}

ZMK_DISPLAY_WIDGET_LISTENER(clean_output_status, struct output_status_state, output_status_update,
                            output_status_get_state)
ZMK_SUBSCRIPTION(clean_output_status, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(clean_output_status, zmk_ble_active_profile_changed);

static void style_plain_label(lv_obj_t *label, const lv_font_t *font) {
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(label, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
}

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);

    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);

    left_battery_label = lv_label_create(screen);
    output_label = lv_label_create(screen);
    right_battery_label = lv_label_create(screen);
    layer_label = lv_label_create(screen);

    style_plain_label(left_battery_label, &lv_font_unscii_8);
    style_plain_label(output_label, &lv_font_unscii_8);
    style_plain_label(right_battery_label, &lv_font_unscii_8);
    style_plain_label(layer_label, &lv_font_montserrat_32);

    lv_obj_align(left_battery_label, LV_ALIGN_TOP_LEFT, 2, 3);
    lv_obj_align(right_battery_label, LV_ALIGN_TOP_RIGHT, -2, 3);

    clean_layer_status_init();
    clean_battery_status_init();
    clean_output_status_init();

    return screen;
}
