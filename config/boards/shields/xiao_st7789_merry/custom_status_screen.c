/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <string.h>

#include <lvgl.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/led.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/modifiers.h>
#include <zmk/activity.h>
#include <zmk/display.h>
#include <zmk/display/status_screen.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/split/central.h>

#include "merry_config.h"
#include "pet_store.h"

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
#define BACKLIGHT_NODE DT_CHOSEN(zmk_display_led)
#define BATTERY_LEFT_SLOT 0u
#define BATTERY_DIAL_SLOT 2u
#define BATTERY_RIGHT_SLOT 1u
#define PET_SCREEN_WIDTH 240
#define PET_SCREEN_HEIGHT 240
#define PET_RENDER_WIDTH 202u
#define PET_RENDER_HEIGHT 220u

BUILD_ASSERT(CONFIG_LV_Z_MEM_POOL_SIZE >= 16000,
             "Merry requires a 16 KB LVGL object pool");
BUILD_ASSERT(CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS >= 3,
             "Merry's L/D/R battery row requires three split peripherals");
BUILD_ASSERT(PET_RENDER_WIDTH <= PET_SCREEN_WIDTH && PET_RENDER_HEIGHT <= PET_SCREEN_HEIGHT,
             "Merry render surface must fit the panel");

static const struct device *const display = DEVICE_DT_GET(DISPLAY_NODE);
static const struct device *const backlight = DEVICE_DT_GET(DT_PARENT(BACKLIGHT_NODE));
static const uint8_t backlight_index = DT_NODE_CHILD_IDX(BACKLIGHT_NODE);

static lv_obj_t *dashboard_battery;
static lv_obj_t *dashboard_layer_caption;
static lv_obj_t *dashboard_layer_badge;
static lv_obj_t *dashboard_layer;
static lv_obj_t *dashboard_mod_caption;
static lv_obj_t *dashboard_modifiers;
static lv_obj_t *dashboard_rule;
static lv_obj_t *pet_image;

static lv_style_t screen_style;
static lv_style_t battery_style;
static lv_style_t caption_style;
static lv_style_t layer_badge_style;
static lv_style_t layer_style;
static lv_style_t modifier_style;
static lv_style_t rule_style;

static uint8_t packed_frame[PET_FRAME_PACKED_BYTES];
/* The QSPI pack stores the native 192x208 art. Decode directly into a larger
 * nearest-neighbour surface so no second source framebuffer is required.
 */
static uint16_t decoded_frame[PET_RENDER_WIDTH * PET_RENDER_HEIGHT];
static lv_image_dsc_t pet_image_descriptor = {
    .header =
        {
            .magic = LV_IMAGE_HEADER_MAGIC,
            .cf = LV_COLOR_FORMAT_RGB565,
            .w = PET_RENDER_WIDTH,
            .h = PET_RENDER_HEIGHT,
            .stride = PET_RENDER_WIDTH * sizeof(uint16_t),
        },
    .data_size = sizeof(decoded_frame),
    .data = (const uint8_t *)decoded_frame,
};

static lv_timer_t *pet_timer;
static struct merry_config ui_config;
static uint8_t pet_animation_id = MERRY_ANIM_IDLE;
static uint16_t pet_animation_frame;
static uint16_t pet_animation_count = 1u;
static bool pet_animation_loop = true;
static bool screen_is_on = true;
static bool pet_mode;
static bool ui_initialized;
static atomic_t requested_mode = ATOMIC_INIT(ZMK_ACTIVITY_ACTIVE);

static uint8_t packed_palette_index(uint32_t source_pixel) {
    const uint32_t bit_index = source_pixel * PET_BITS_PER_PIXEL;
    const size_t byte_index = bit_index >> 3;
    const uint8_t bit_shift = bit_index & 7u;
    uint16_t value = packed_frame[byte_index];
    if (bit_shift + PET_BITS_PER_PIXEL > 8u) {
        value |= (uint16_t)packed_frame[byte_index + 1u] << 8;
    }
    return (value >> bit_shift) & (PET_PALETTE_SIZE - 1u);
}

static void set_hidden(lv_obj_t *object, bool hidden) {
    if (hidden) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_dashboard_visible(bool visible) {
    set_hidden(dashboard_battery, !visible);
    set_hidden(dashboard_layer_caption, !visible);
    set_hidden(dashboard_layer_badge, !visible);
    set_hidden(dashboard_layer, !visible);
    set_hidden(dashboard_mod_caption, !visible);
    set_hidden(dashboard_modifiers, !visible);
    set_hidden(dashboard_rule, !visible);
}

static void set_screen_power(bool on) {
    if (on) {
        if (!screen_is_on) {
            display_blanking_off(display);
        }
        (void)led_set_brightness(backlight, backlight_index, ui_config.brightness);
        screen_is_on = true;
        if (pet_mode && pet_timer != NULL) {
            lv_timer_resume(pet_timer);
        }
    } else if (screen_is_on) {
        if (pet_timer != NULL) {
            lv_timer_pause(pet_timer);
        }
        display_blanking_on(display);
        led_off(backlight, backlight_index);
        screen_is_on = false;
    }
}

static int load_pet_frame(uint16_t animation_frame) {
    struct pet_frame frame;
    int rc = pet_store_read_frame(pet_animation_id, animation_frame, &frame, packed_frame,
                                  sizeof(packed_frame));
    if (rc < 0) {
        return rc;
    }

    for (uint32_t y = 0u; y < PET_RENDER_HEIGHT; y++) {
        const uint32_t source_y =
            ((2u * y + 1u) * PET_FRAME_HEIGHT) / (2u * PET_RENDER_HEIGHT);
        for (uint32_t x = 0u; x < PET_RENDER_WIDTH; x++) {
            const uint32_t source_x =
                ((2u * x + 1u) * PET_FRAME_WIDTH) / (2u * PET_RENDER_WIDTH);
            const uint32_t source_pixel = source_y * PET_FRAME_WIDTH + source_x;
            const uint8_t palette_index = packed_palette_index(source_pixel);
            decoded_frame[y * PET_RENDER_WIDTH + x] = frame.palette[palette_index];
        }
    }

    lv_obj_invalidate(pet_image);
    if (pet_timer != NULL) {
        lv_timer_set_period(pet_timer, frame.duration_ms);
    }
    return 0;
}

static void pet_timer_callback(lv_timer_t *timer) {
    ARG_UNUSED(timer);
    if (!pet_mode || !screen_is_on) {
        return;
    }

    if (pet_animation_frame + 1u >= pet_animation_count) {
        if (!pet_animation_loop) {
            lv_timer_pause(timer);
            return;
        }
        pet_animation_frame = 0u;
    } else {
        pet_animation_frame++;
    }
    if (load_pet_frame(pet_animation_frame) < 0) {
        pet_animation_frame = 0u;
        pet_animation_count = 1u;
        (void)load_pet_frame(0u);
    }
}

static void refresh_pet_animation(void) {
    struct pet_animation_desc animation;
    pet_animation_id = ui_config.animation_id;
    if (pet_store_get_animation(pet_animation_id, &animation) == 0) {
        pet_animation_count = animation.frame_count;
        pet_animation_loop = (animation.flags & 1u) != 0u;
    } else {
        pet_animation_id = MERRY_ANIM_IDLE;
        pet_animation_count =
            pet_store_get_animation(pet_animation_id, &animation) == 0
                ? animation.frame_count
                : 1u;
        pet_animation_loop = true;
    }
    pet_animation_frame = 0u;
    (void)load_pet_frame(0u);
}

static void show_dashboard(void) {
    pet_mode = false;
    set_screen_power(true);
    if (pet_timer != NULL) {
        lv_timer_pause(pet_timer);
    }
    set_hidden(pet_image, true);
    set_dashboard_visible(true);
}

static void show_pet(void) {
    pet_mode = true;
    set_screen_power(true);
    set_dashboard_visible(false);
    set_hidden(pet_image, false);
    lv_obj_align(pet_image, LV_ALIGN_CENTER, ui_config.pet_x, ui_config.pet_y);
    refresh_pet_animation();
    if (pet_timer != NULL) {
        lv_timer_resume(pet_timer);
    }
}

static void screen_off_ui_work_callback(struct k_work *work) {
    ARG_UNUSED(work);
    if (ui_config.display_mode == MERRY_DISPLAY_AUTO && pet_mode &&
        atomic_get(&requested_mode) == ZMK_ACTIVITY_IDLE &&
        zmk_activity_get_state() == ZMK_ACTIVITY_IDLE) {
        set_screen_power(false);
    }
}

K_WORK_DEFINE(screen_off_ui_work, screen_off_ui_work_callback);

static void screen_off_delay_callback(struct k_work *work) {
    ARG_UNUSED(work);
    if (ui_config.display_mode == MERRY_DISPLAY_AUTO && pet_mode &&
        atomic_get(&requested_mode) == ZMK_ACTIVITY_IDLE &&
        zmk_activity_get_state() == ZMK_ACTIVITY_IDLE) {
        k_work_submit_to_queue(zmk_display_work_q(), &screen_off_ui_work);
    }
}

K_WORK_DELAYABLE_DEFINE(screen_off_delay_work, screen_off_delay_callback);

static void pet_delay_ui_work_callback(struct k_work *work) {
    ARG_UNUSED(work);
    if (ui_config.display_mode == MERRY_DISPLAY_AUTO &&
        atomic_get(&requested_mode) == ZMK_ACTIVITY_IDLE &&
        zmk_activity_get_state() == ZMK_ACTIVITY_IDLE) {
        show_pet();
        k_work_reschedule(&screen_off_delay_work,
                          K_MSEC(CONFIG_MERRY_SCREEN_OFF_DELAY_MS));
    }
}

K_WORK_DEFINE(pet_delay_ui_work, pet_delay_ui_work_callback);

static void pet_delay_callback(struct k_work *work) {
    ARG_UNUSED(work);
    if (ui_config.display_mode == MERRY_DISPLAY_AUTO &&
        atomic_get(&requested_mode) == ZMK_ACTIVITY_IDLE &&
        zmk_activity_get_state() == ZMK_ACTIVITY_IDLE) {
        k_work_submit_to_queue(zmk_display_work_q(), &pet_delay_ui_work);
    }
}

K_WORK_DELAYABLE_DEFINE(pet_delay_work, pet_delay_callback);

static void schedule_pet_after(uint32_t delay_ms) {
    (void)k_work_cancel_delayable(&screen_off_delay_work);
    if (delay_ms == 0u) {
        pet_delay_ui_work_callback(NULL);
    } else {
        k_work_reschedule(&pet_delay_work, K_MSEC(delay_ms));
    }
}

static void config_apply_work_callback(struct k_work *work) {
    ARG_UNUSED(work);

    if (merry_config_get(&ui_config) < 0 || !ui_initialized) {
        return;
    }

    (void)k_work_cancel_delayable(&pet_delay_work);
    (void)k_work_cancel_delayable(&screen_off_delay_work);
    lv_obj_align(pet_image, LV_ALIGN_CENTER, ui_config.pet_x, ui_config.pet_y);

    switch (ui_config.display_mode) {
    case MERRY_DISPLAY_DASHBOARD:
        show_dashboard();
        break;
    case MERRY_DISPLAY_PET:
        show_pet();
        break;
    case MERRY_DISPLAY_OFF:
        set_screen_power(false);
        break;
    case MERRY_DISPLAY_AUTO:
    default:
        if (zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE) {
            show_dashboard();
        } else if (zmk_activity_get_state() == ZMK_ACTIVITY_IDLE) {
            show_dashboard();
            schedule_pet_after(ui_config.idle_timeout_ms);
        } else {
            set_screen_power(false);
        }
        break;
    }
}

K_WORK_DEFINE(config_apply_work, config_apply_work_callback);

void merry_screen_config_changed(void) {
    if (ui_initialized && zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &config_apply_work);
    }
}

struct activity_status_state {
    enum zmk_activity_state state;
};

static struct activity_status_state activity_status_get_state(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *event = as_zmk_activity_state_changed(eh);
    return (struct activity_status_state){
        .state = event == NULL ? zmk_activity_get_state() : event->state,
    };
}

static void activity_status_update(struct activity_status_state state) {
    atomic_set(&requested_mode, state.state);
    if (ui_config.display_mode != MERRY_DISPLAY_AUTO) {
        return;
    }
    if (state.state == ZMK_ACTIVITY_ACTIVE) {
        (void)k_work_cancel_delayable(&pet_delay_work);
        (void)k_work_cancel_delayable(&screen_off_delay_work);
        show_dashboard();
    } else if (state.state == ZMK_ACTIVITY_IDLE) {
        show_dashboard();
        const uint32_t already_idle_ms = CONFIG_ZMK_IDLE_TIMEOUT;
        schedule_pet_after(ui_config.idle_timeout_ms > already_idle_ms
                               ? ui_config.idle_timeout_ms - already_idle_ms
                               : 0u);
    } else {
        (void)k_work_cancel_delayable(&pet_delay_work);
        (void)k_work_cancel_delayable(&screen_off_delay_work);
        set_screen_power(false);
    }
}

ZMK_DISPLAY_WIDGET_LISTENER(merry_activity_status, struct activity_status_state,
                            activity_status_update, activity_status_get_state)
ZMK_SUBSCRIPTION(merry_activity_status, zmk_activity_state_changed);

struct layer_status_state {
    const char *name;
};

static struct layer_status_state layer_status_get_state(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    const char *name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index));
    return (struct layer_status_state){.name = (name != NULL && name[0] != '\0') ? name : "BASE"};
}

static void layer_status_update(struct layer_status_state state) {
    lv_label_set_text(dashboard_layer, state.name);
    lv_obj_align(dashboard_layer, LV_ALIGN_CENTER, 0, -4);
}

ZMK_DISPLAY_WIDGET_LISTENER(merry_layer_status, struct layer_status_state, layer_status_update,
                            layer_status_get_state)
ZMK_SUBSCRIPTION(merry_layer_status, zmk_layer_state_changed);

struct peripheral_battery_state {
    uint8_t left;
    uint8_t dial;
    uint8_t right;
};

static struct peripheral_battery_state peripheral_battery_get_state(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    struct peripheral_battery_state state = {};
    (void)zmk_split_central_get_peripheral_battery_level(BATTERY_LEFT_SLOT, &state.left);
    (void)zmk_split_central_get_peripheral_battery_level(BATTERY_DIAL_SLOT, &state.dial);
    (void)zmk_split_central_get_peripheral_battery_level(BATTERY_RIGHT_SLOT, &state.right);
    return state;
}

static void battery_value(char *destination, size_t size, uint8_t level) {
    if (level == 0u) {
        snprintf(destination, size, "--");
    } else {
        snprintf(destination, size, "%u", level);
    }
}

static void peripheral_battery_update(struct peripheral_battery_state state) {
    char left[4];
    char dial[4];
    char right[4];
    char text[32];
    battery_value(left, sizeof(left), state.left);
    battery_value(dial, sizeof(dial), state.dial);
    battery_value(right, sizeof(right), state.right);
    snprintf(text, sizeof(text), "L %s   D %s   R %s", left, dial, right);
    lv_label_set_text(dashboard_battery, text);
    lv_obj_align(dashboard_battery, LV_ALIGN_TOP_MID, 0, 13);
}

ZMK_DISPLAY_WIDGET_LISTENER(merry_battery_status, struct peripheral_battery_state,
                            peripheral_battery_update, peripheral_battery_get_state)
ZMK_SUBSCRIPTION(merry_battery_status, zmk_peripheral_battery_state_changed);

static void modifier_update_work_callback(struct k_work *work) {
    ARG_UNUSED(work);
    const zmk_mod_flags_t mods = zmk_hid_get_explicit_mods();
    char text[24] = "";
    if ((mods & (MOD_LCTL | MOD_RCTL)) != 0u) {
        strcat(text, "CTRL");
    }
    if ((mods & (MOD_LALT | MOD_RALT)) != 0u) {
        strcat(text, text[0] == '\0' ? "ALT" : "  ALT");
    }
    if ((mods & (MOD_LSFT | MOD_RSFT)) != 0u) {
        strcat(text, text[0] == '\0' ? "SHIFT" : "  SHIFT");
    }
    lv_label_set_text(dashboard_modifiers, text);
    lv_obj_align(dashboard_modifiers, LV_ALIGN_BOTTOM_MID, 0, -13);
}

K_WORK_DEFINE(modifier_update_work, modifier_update_work_callback);

static int modifier_event_handler(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &modifier_update_work);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(merry_modifiers, modifier_event_handler);
ZMK_SUBSCRIPTION(merry_modifiers, zmk_keycode_state_changed);

static void init_styles(void) {
    lv_style_init(&screen_style);
    /* Pet transparency is encoded as RGB565 black, so a true-black screen
     * background makes the sprite boundary disappear without an alpha buffer.
     */
    lv_style_set_bg_color(&screen_style, lv_color_hex(0x000000));
    lv_style_set_bg_opa(&screen_style, LV_OPA_COVER);
    lv_style_set_border_width(&screen_style, 0);
    lv_style_set_pad_all(&screen_style, 0);

    lv_style_init(&battery_style);
    lv_style_set_text_font(&battery_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&battery_style, lv_color_hex(0xf4eee4));
    lv_style_set_bg_opa(&battery_style, LV_OPA_TRANSP);

    lv_style_init(&caption_style);
    lv_style_set_text_font(&caption_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&caption_style, lv_color_hex(0x9b948b));
    lv_style_set_bg_opa(&caption_style, LV_OPA_TRANSP);

    lv_style_init(&layer_style);
    lv_style_set_text_font(&layer_style, &lv_font_montserrat_32);
    lv_style_set_text_color(&layer_style, lv_color_hex(0xc084fc));
    lv_style_set_text_letter_space(&layer_style, 2);
    lv_style_set_bg_opa(&layer_style, LV_OPA_TRANSP);

    lv_style_init(&layer_badge_style);
    lv_style_set_bg_color(&layer_badge_style, lv_color_hex(0x251036));
    lv_style_set_bg_opa(&layer_badge_style, LV_OPA_COVER);
    lv_style_set_border_color(&layer_badge_style, lv_color_hex(0x9b5de5));
    lv_style_set_border_width(&layer_badge_style, 2);
    lv_style_set_radius(&layer_badge_style, 12);
    lv_style_set_pad_all(&layer_badge_style, 0);

    lv_style_init(&modifier_style);
    lv_style_set_text_font(&modifier_style, &lv_font_montserrat_20);
    lv_style_set_text_color(&modifier_style, lv_color_hex(0xf4eee4));
    lv_style_set_bg_opa(&modifier_style, LV_OPA_TRANSP);

    lv_style_init(&rule_style);
    lv_style_set_bg_color(&rule_style, lv_color_hex(0x3a302a));
    lv_style_set_bg_opa(&rule_style, LV_OPA_COVER);
    lv_style_set_border_width(&rule_style, 0);
    lv_style_set_radius(&rule_style, 1);
}

lv_obj_t *zmk_display_status_screen(void) {
    init_styles();
    (void)pet_store_init();
    (void)merry_config_get(&ui_config);

    lv_obj_t *screen = lv_obj_create(NULL);
    if (screen == NULL) {
        return NULL;
    }
    lv_obj_add_style(screen, &screen_style, LV_PART_MAIN);

    dashboard_battery = lv_label_create(screen);
    dashboard_layer_caption = lv_label_create(screen);
    dashboard_layer_badge = lv_obj_create(screen);
    dashboard_layer = lv_label_create(screen);
    dashboard_mod_caption = lv_label_create(screen);
    dashboard_modifiers = lv_label_create(screen);
    dashboard_rule = lv_obj_create(screen);
    pet_image = lv_image_create(screen);
    if (dashboard_battery == NULL || dashboard_layer_caption == NULL ||
        dashboard_layer_badge == NULL || dashboard_layer == NULL || dashboard_mod_caption == NULL ||
        dashboard_modifiers == NULL || dashboard_rule == NULL || pet_image == NULL) {
        lv_obj_del(screen);
        return NULL;
    }

    lv_obj_add_style(dashboard_battery, &battery_style, LV_PART_MAIN);
    lv_obj_add_style(dashboard_layer_caption, &caption_style, LV_PART_MAIN);
    lv_obj_add_style(dashboard_layer_badge, &layer_badge_style, LV_PART_MAIN);
    lv_obj_add_style(dashboard_layer, &layer_style, LV_PART_MAIN);
    lv_obj_add_style(dashboard_mod_caption, &caption_style, LV_PART_MAIN);
    lv_obj_add_style(dashboard_modifiers, &modifier_style, LV_PART_MAIN);
    lv_obj_add_style(dashboard_rule, &rule_style, LV_PART_MAIN);

    lv_label_set_text(dashboard_layer_caption, "LAYER");
    lv_obj_align(dashboard_layer_caption, LV_ALIGN_CENTER, 0, -48);
    lv_obj_set_size(dashboard_layer_badge, 190, 58);
    lv_obj_align(dashboard_layer_badge, LV_ALIGN_CENTER, 0, -4);
    lv_label_set_text(dashboard_mod_caption, "MODIFIERS");
    lv_obj_align(dashboard_mod_caption, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_obj_set_size(dashboard_rule, 200, 2);
    lv_obj_align(dashboard_rule, LV_ALIGN_TOP_MID, 0, 49);

    lv_image_set_src(pet_image, &pet_image_descriptor);
    lv_obj_align(pet_image, LV_ALIGN_CENTER, ui_config.pet_x, ui_config.pet_y);
    set_hidden(pet_image, true);

    pet_timer = lv_timer_create(pet_timer_callback, 250, NULL);
    if (pet_timer == NULL) {
        lv_obj_del(screen);
        return NULL;
    }
    lv_timer_pause(pet_timer);

    ui_initialized = true;
    merry_layer_status_init();
    merry_battery_status_init();
    modifier_update_work_callback(NULL);
    merry_activity_status_init();
    config_apply_work_callback(NULL);

    return screen;
}
