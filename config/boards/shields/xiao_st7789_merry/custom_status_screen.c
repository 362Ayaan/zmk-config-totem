/* SPDX-License-Identifier: MIT */

#include <errno.h>
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
#include "merry_codex_state.h"
#include "merry_host_activity.h"
#include "merry_media.h"
#include "pet_store.h"

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
#define BACKLIGHT_NODE DT_CHOSEN(zmk_display_led)
#define PET_SCREEN_WIDTH 240
#define PET_SCREEN_HEIGHT 240
#define PET_RENDER_WIDTH MERRY_MEDIA_WIDTH
#define PET_RENDER_HEIGHT MERRY_MEDIA_HEIGHT

BUILD_ASSERT(CONFIG_LV_Z_MEM_POOL_SIZE >= 16000,
             "Merry requires a 16 KB LVGL object pool");
BUILD_ASSERT(CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS >= 3,
             "Merry's L/D/R battery row requires three split peripherals");
BUILD_ASSERT(PET_RENDER_WIDTH <= PET_SCREEN_WIDTH && PET_RENDER_HEIGHT <= PET_SCREEN_HEIGHT,
             "Merry render surface must fit the panel");

static const struct device *const display = DEVICE_DT_GET(DISPLAY_NODE);
static const struct device *const backlight = DEVICE_DT_GET(DT_PARENT(BACKLIGHT_NODE));
static const uint8_t backlight_index = DT_NODE_CHILD_IDX(BACKLIGHT_NODE);

static lv_obj_t *dashboard_battery_left;
static lv_obj_t *dashboard_battery_dial;
static lv_obj_t *dashboard_battery_right;
static lv_obj_t *dashboard_layer_accent;
static lv_obj_t *dashboard_layer;
static lv_obj_t *dashboard_mod_ctrl;
static lv_obj_t *dashboard_mod_alt;
static lv_obj_t *dashboard_mod_shift;
static lv_obj_t *dashboard_rule;
static lv_obj_t *pet_image;
static lv_obj_t *media_badge;
static lv_obj_t *media_play;
static lv_obj_t *media_pause_left;
static lv_obj_t *media_pause_right;

static lv_style_t screen_style;
static lv_style_t battery_style;
static lv_style_t layer_accent_style;
static lv_style_t layer_style;
static lv_style_t modifier_style;
static lv_style_t rule_style;
static lv_style_t media_badge_style;
static lv_style_t media_play_style;
static lv_style_t media_pause_style;

static lv_point_precise_t media_play_points[] = {
    {0, 0},
    {0, 20},
    {17, 10},
    {0, 0},
};

static uint8_t packed_frame[PET_FRAME_PACKED_BYTES] __aligned(4);
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
static bool media_mode;
static bool ui_initialized;
static atomic_t requested_mode = ATOMIC_INIT(ZMK_ACTIVITY_ACTIVE);
static atomic_t codex_animation_id = ATOMIC_INIT(MERRY_ANIM_IDLE);
static atomic_t media_state = ATOMIC_INIT(MERRY_MEDIA_NONE);
static atomic_t media_valid = ATOMIC_INIT(0);
static atomic_t host_state = ATOMIC_INIT(MERRY_HOST_AFK);
K_SEM_DEFINE(media_ui_sync, 0, 1);

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
    set_hidden(dashboard_battery_left, !visible);
    set_hidden(dashboard_battery_dial, !visible);
    set_hidden(dashboard_battery_right, !visible);
    set_hidden(dashboard_layer_accent, !visible);
    set_hidden(dashboard_layer, !visible);
    set_hidden(dashboard_mod_ctrl, !visible);
    set_hidden(dashboard_mod_alt, !visible);
    set_hidden(dashboard_mod_shift, !visible);
    set_hidden(dashboard_rule, !visible);
}

static void set_media_overlay_visible(bool visible) {
    set_hidden(media_badge, !visible);
    if (!visible) {
        set_hidden(media_play, true);
        set_hidden(media_pause_left, true);
        set_hidden(media_pause_right, true);
        return;
    }
    const bool playing = atomic_get(&media_state) == MERRY_MEDIA_PLAYING;
    set_hidden(media_play, playing);
    set_hidden(media_pause_left, !playing);
    set_hidden(media_pause_right, !playing);
}

static void set_screen_power(bool on) {
    if (on) {
        if (!screen_is_on) {
            display_blanking_off(display);
        }
        (void)led_set_brightness(backlight, backlight_index, ui_config.brightness);
        screen_is_on = true;
        if (pet_mode && !media_mode && pet_timer != NULL) {
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

    atomic_clear(&media_valid);

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
    pet_animation_id = ui_config.display_mode == MERRY_DISPLAY_AUTO
                           ? (uint8_t)atomic_get(&codex_animation_id)
                           : ui_config.animation_id;
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

static bool media_should_show(void) {
    const uint8_t codex = (uint8_t)atomic_get(&codex_animation_id);
    const uint8_t media = (uint8_t)atomic_get(&media_state);

    /* Keep this priority rule identical to Test-MerryMediaEligible in the
     * Windows bridge.  In particular, paused artwork remains visible after a
     * Completed/NeedsInput/Blocked Codex state.  Falling through to show_pet()
     * would decode a pet frame into the shared render buffer, permanently
     * discarding the album until the host performs another full upload. */
    return atomic_get(&media_valid) && codex != MERRY_ANIM_RUNNING &&
           (media == MERRY_MEDIA_PLAYING || media == MERRY_MEDIA_PAUSED);
}

static void show_pet(void);
static void config_apply_work_callback(struct k_work *work);

static void show_media(void) {
    pet_mode = true;
    media_mode = true;
    set_screen_power(true);
    set_dashboard_visible(false);
    if (pet_timer != NULL) {
        lv_timer_pause(pet_timer);
    }
    set_hidden(pet_image, false);
    set_media_overlay_visible(true);
    lv_obj_align(pet_image, LV_ALIGN_CENTER, ui_config.pet_x, ui_config.pet_y);
    lv_obj_align(media_badge, LV_ALIGN_CENTER, ui_config.pet_x + 70, ui_config.pet_y + 71);
    lv_obj_align(media_play, LV_ALIGN_CENTER, ui_config.pet_x + 71, ui_config.pet_y + 71);
    lv_obj_align(media_pause_left, LV_ALIGN_CENTER, ui_config.pet_x + 64, ui_config.pet_y + 71);
    lv_obj_align(media_pause_right, LV_ALIGN_CENTER, ui_config.pet_x + 76, ui_config.pet_y + 71);
    lv_obj_invalidate(pet_image);
}

static void show_idle_content(void) {
    if (media_should_show()) {
        show_media();
    } else {
        show_pet();
    }
}

static void codex_state_apply_work_callback(struct k_work *work) {
    ARG_UNUSED(work);

    /* Codex chooses the animation only in adaptive/automatic mode. Keyboard
     * activity continues to own dashboard, pet, and screen-off transitions.
     * In particular, a background Codex task never wakes an AFK display.
     */
    if (ui_initialized && ui_config.display_mode == MERRY_DISPLAY_AUTO && pet_mode &&
        screen_is_on) {
        if (media_should_show()) {
            show_media();
        } else {
            show_pet();
        }
    }
}

K_WORK_DEFINE(codex_state_apply_work, codex_state_apply_work_callback);

uint8_t merry_codex_state_get(void) {
    return (uint8_t)atomic_get(&codex_animation_id);
}

int merry_codex_state_set(uint8_t animation_id) {
    if (animation_id > MERRY_ANIM_BLOCKED) {
        return -EINVAL;
    }

    atomic_val_t previous = atomic_set(&codex_animation_id, animation_id);
    if ((uint8_t)previous != animation_id && ui_initialized && zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &codex_state_apply_work);
    }
    return 0;
}

static void media_state_apply_work_callback(struct k_work *work) {
    ARG_UNUSED(work);

    if (!ui_initialized || ui_config.display_mode != MERRY_DISPLAY_AUTO || !pet_mode ||
        !screen_is_on) {
        return;
    }
    show_idle_content();
}

K_WORK_DEFINE(media_state_apply_work, media_state_apply_work_callback);

static void media_upload_begin_work_callback(struct k_work *work) {
    ARG_UNUSED(work);

    if (pet_timer != NULL) {
        lv_timer_pause(pet_timer);
    }
    if (pet_mode) {
        set_hidden(pet_image, true);
        set_media_overlay_visible(false);
    }
    media_mode = false;
    atomic_clear(&media_valid);
    k_sem_give(&media_ui_sync);
}

K_WORK_DEFINE(media_upload_begin_work, media_upload_begin_work_callback);

uint8_t merry_media_state_get(void) {
    return (uint8_t)atomic_get(&media_state);
}

int merry_media_state_set(uint8_t state) {
    if (state > MERRY_MEDIA_PAUSED) {
        return -EINVAL;
    }

    atomic_val_t previous = atomic_set(&media_state, state);
    if ((uint8_t)previous != state && ui_initialized && zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &media_state_apply_work);
    }
    return 0;
}

int merry_media_upload_begin(void) {
    if (!ui_initialized || !zmk_display_is_initialized()) {
        atomic_clear(&media_valid);
        return 0;
    }

    k_sem_reset(&media_ui_sync);
    if (k_work_submit_to_queue(zmk_display_work_q(), &media_upload_begin_work) < 0) {
        return -EIO;
    }
    return k_sem_take(&media_ui_sync, K_SECONDS(2));
}

int merry_media_upload_write(size_t offset, const uint8_t *data, size_t size) {
    if (data == NULL || offset > sizeof(decoded_frame) || size > sizeof(decoded_frame) - offset) {
        return -EINVAL;
    }
    memcpy((uint8_t *)decoded_frame + offset, data, size);
    return 0;
}

int merry_media_upload_finish(uint32_t expected_crc32, uint8_t state) {
    if (state < MERRY_MEDIA_PLAYING || state > MERRY_MEDIA_PAUSED ||
        pet_crc32((const uint8_t *)decoded_frame, sizeof(decoded_frame)) != expected_crc32) {
        merry_media_upload_abort();
        return -EINVAL;
    }

    atomic_set(&media_state, state);
    atomic_set(&media_valid, 1);
    if (ui_initialized && zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &media_state_apply_work);
    }
    return 0;
}

void merry_media_upload_abort(void) {
    atomic_clear(&media_valid);
    if (ui_initialized && zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &media_state_apply_work);
    }
}

static void show_dashboard(void) {
    pet_mode = false;
    media_mode = false;
    set_screen_power(true);
    if (pet_timer != NULL) {
        lv_timer_pause(pet_timer);
    }
    set_hidden(pet_image, true);
    set_media_overlay_visible(false);
    set_dashboard_visible(true);
}

static void show_pet(void) {
    pet_mode = true;
    media_mode = false;
    set_screen_power(true);
    set_dashboard_visible(false);
    set_hidden(pet_image, false);
    set_media_overlay_visible(false);
    lv_obj_align(pet_image, LV_ALIGN_CENTER, ui_config.pet_x, ui_config.pet_y);
    lv_obj_align(media_badge, LV_ALIGN_CENTER, ui_config.pet_x + 70, ui_config.pet_y + 71);
    lv_obj_align(media_play, LV_ALIGN_CENTER, ui_config.pet_x + 71, ui_config.pet_y + 71);
    lv_obj_align(media_pause_left, LV_ALIGN_CENTER, ui_config.pet_x + 64, ui_config.pet_y + 71);
    lv_obj_align(media_pause_right, LV_ALIGN_CENTER, ui_config.pet_x + 76, ui_config.pet_y + 71);
    refresh_pet_animation();
    if (pet_timer != NULL) {
        lv_timer_resume(pet_timer);
    }
}

static void screen_off_ui_work_callback(struct k_work *work) {
    ARG_UNUSED(work);
    if (ui_config.display_mode == MERRY_DISPLAY_AUTO && pet_mode &&
        atomic_get(&requested_mode) == ZMK_ACTIVITY_IDLE &&
        zmk_activity_get_state() == ZMK_ACTIVITY_IDLE &&
        atomic_get(&host_state) != MERRY_HOST_ACTIVE) {
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

static void host_activity_apply_work_callback(struct k_work *work) {
    ARG_UNUSED(work);

    if (!ui_initialized) {
        return;
    }

    const uint8_t state = (uint8_t)atomic_get(&host_state);
    if (state == MERRY_HOST_DISPLAY_OFF) {
        (void)k_work_cancel_delayable(&pet_delay_work);
        (void)k_work_cancel_delayable(&screen_off_delay_work);
        set_screen_power(false);
        return;
    }

    if (ui_config.display_mode != MERRY_DISPLAY_AUTO) {
        config_apply_work_callback(NULL);
        return;
    }

    if (state == MERRY_HOST_ACTIVE) {
        if (!screen_is_on) {
            if (zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE ||
                atomic_get(&requested_mode) == ZMK_ACTIVITY_ACTIVE) {
                show_dashboard();
            } else {
                show_idle_content();
            }
        }
        if (pet_mode) {
            k_work_reschedule(&screen_off_delay_work,
                              K_MSEC(CONFIG_MERRY_SCREEN_OFF_DELAY_MS));
        }
    }
}

K_WORK_DEFINE(host_activity_apply_work, host_activity_apply_work_callback);

uint8_t merry_host_state_get(void) {
    return (uint8_t)atomic_get(&host_state);
}

int merry_host_state_set(uint8_t state) {
    if (state > MERRY_HOST_DISPLAY_OFF) {
        return -EINVAL;
    }
    atomic_val_t previous = atomic_set(&host_state, state);
    if ((previous != state || state != MERRY_HOST_AFK) && ui_initialized &&
        zmk_display_is_initialized()) {
        /* Every active heartbeat moves the AFK deadline forward. Display-off
         * is an absolute override and never wakes from keyboard activity. */
        k_work_submit_to_queue(zmk_display_work_q(), &host_activity_apply_work);
    }
    return 0;
}

static void pet_delay_ui_work_callback(struct k_work *work) {
    ARG_UNUSED(work);
    if (ui_config.display_mode == MERRY_DISPLAY_AUTO &&
        atomic_get(&host_state) != MERRY_HOST_DISPLAY_OFF &&
        atomic_get(&requested_mode) == ZMK_ACTIVITY_IDLE &&
        zmk_activity_get_state() == ZMK_ACTIVITY_IDLE) {
        show_idle_content();
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
    lv_obj_align(media_badge, LV_ALIGN_CENTER, ui_config.pet_x + 70, ui_config.pet_y + 71);
    lv_obj_align(media_play, LV_ALIGN_CENTER, ui_config.pet_x + 71, ui_config.pet_y + 71);
    lv_obj_align(media_pause_left, LV_ALIGN_CENTER, ui_config.pet_x + 64, ui_config.pet_y + 71);
    lv_obj_align(media_pause_right, LV_ALIGN_CENTER, ui_config.pet_x + 76, ui_config.pet_y + 71);

    if (merry_host_state_get() == MERRY_HOST_DISPLAY_OFF) {
        set_screen_power(false);
        return;
    }

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
    if (merry_host_state_get() == MERRY_HOST_DISPLAY_OFF) {
        (void)k_work_cancel_delayable(&pet_delay_work);
        (void)k_work_cancel_delayable(&screen_off_delay_work);
        set_screen_power(false);
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
    lv_obj_align(dashboard_layer, LV_ALIGN_CENTER, 8, -5);
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
    (void)zmk_split_central_get_peripheral_battery_level(ui_config.battery_left_slot,
                                                         &state.left);
    (void)zmk_split_central_get_peripheral_battery_level(ui_config.battery_dial_slot,
                                                         &state.dial);
    (void)zmk_split_central_get_peripheral_battery_level(ui_config.battery_right_slot,
                                                         &state.right);
    return state;
}

static void battery_value(char *destination, size_t size, char label, uint8_t level) {
    if (level == 0u) {
        snprintf(destination, size, "%c --", label);
    } else {
        snprintf(destination, size, "%c %u", label, level);
    }
}

static lv_color_t battery_color(uint8_t level) {
    if (level == 0u) {
        return lv_color_hex(0x5f5964);
    }
    if (level <= 20u) {
        return lv_color_hex(0xff453a);
    }
    if (level <= 50u) {
        return lv_color_hex(0xff9f0a);
    }
    return lv_color_hex(0x32d74b);
}

static void peripheral_battery_update(struct peripheral_battery_state state) {
    char left[8];
    char dial[8];
    char right[8];
    battery_value(left, sizeof(left), 'L', state.left);
    battery_value(dial, sizeof(dial), 'D', state.dial);
    battery_value(right, sizeof(right), 'R', state.right);
    lv_label_set_text(dashboard_battery_left, left);
    lv_label_set_text(dashboard_battery_dial, dial);
    lv_label_set_text(dashboard_battery_right, right);
    lv_obj_set_style_text_color(dashboard_battery_left, battery_color(state.left), LV_PART_MAIN);
    lv_obj_set_style_text_color(dashboard_battery_dial, battery_color(state.dial), LV_PART_MAIN);
    lv_obj_set_style_text_color(dashboard_battery_right, battery_color(state.right), LV_PART_MAIN);
    lv_obj_align(dashboard_battery_left, LV_ALIGN_TOP_MID, -76, 14);
    lv_obj_align(dashboard_battery_dial, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_align(dashboard_battery_right, LV_ALIGN_TOP_MID, 76, 14);
}

ZMK_DISPLAY_WIDGET_LISTENER(merry_battery_status, struct peripheral_battery_state,
                            peripheral_battery_update, peripheral_battery_get_state)
ZMK_SUBSCRIPTION(merry_battery_status, zmk_peripheral_battery_state_changed);

static void modifier_update_work_callback(struct k_work *work) {
    ARG_UNUSED(work);
    const zmk_mod_flags_t mods = zmk_hid_get_explicit_mods();
    const lv_color_t inactive = lv_color_hex(0x49434f);
    const lv_color_t active = lv_color_hex(0xbf5af2);
    lv_obj_set_style_text_color(dashboard_mod_ctrl,
                                (mods & (MOD_LCTL | MOD_RCTL)) != 0u ? active : inactive,
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(dashboard_mod_alt,
                                (mods & (MOD_LALT | MOD_RALT)) != 0u ? active : inactive,
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(dashboard_mod_shift,
                                (mods & (MOD_LSFT | MOD_RSFT)) != 0u ? active : inactive,
                                LV_PART_MAIN);
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

    lv_style_init(&layer_style);
    lv_style_set_text_font(&layer_style, &lv_font_montserrat_32);
    lv_style_set_text_color(&layer_style, lv_color_hex(0xbf5af2));
    lv_style_set_text_letter_space(&layer_style, 2);
    lv_style_set_bg_opa(&layer_style, LV_OPA_TRANSP);

    lv_style_init(&layer_accent_style);
    lv_style_set_bg_color(&layer_accent_style, lv_color_hex(0xbf5af2));
    lv_style_set_bg_opa(&layer_accent_style, LV_OPA_COVER);
    lv_style_set_border_width(&layer_accent_style, 0);
    lv_style_set_radius(&layer_accent_style, 0);
    lv_style_set_pad_all(&layer_accent_style, 0);

    lv_style_init(&modifier_style);
    lv_style_set_text_font(&modifier_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&modifier_style, lv_color_hex(0x49434f));
    lv_style_set_text_letter_space(&modifier_style, 1);
    lv_style_set_bg_opa(&modifier_style, LV_OPA_TRANSP);

    lv_style_init(&rule_style);
    lv_style_set_bg_color(&rule_style, lv_color_hex(0x2c2730));
    lv_style_set_bg_opa(&rule_style, LV_OPA_COVER);
    lv_style_set_border_width(&rule_style, 0);
    lv_style_set_radius(&rule_style, 0);

    lv_style_init(&media_badge_style);
    lv_style_set_bg_color(&media_badge_style, lv_color_hex(0x000000));
    lv_style_set_bg_opa(&media_badge_style, LV_OPA_80);
    lv_style_set_border_color(&media_badge_style, lv_color_hex(0xffffff));
    lv_style_set_border_opa(&media_badge_style, LV_OPA_80);
    lv_style_set_border_width(&media_badge_style, 2);
    lv_style_set_radius(&media_badge_style, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&media_badge_style, 0);

    lv_style_init(&media_play_style);
    lv_style_set_line_color(&media_play_style, lv_color_hex(0xffffff));
    lv_style_set_line_width(&media_play_style, 3);
    lv_style_set_line_rounded(&media_play_style, true);

    lv_style_init(&media_pause_style);
    lv_style_set_bg_color(&media_pause_style, lv_color_hex(0xffffff));
    lv_style_set_bg_opa(&media_pause_style, LV_OPA_COVER);
    lv_style_set_border_width(&media_pause_style, 0);
    lv_style_set_radius(&media_pause_style, 0);
    lv_style_set_pad_all(&media_pause_style, 0);
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

    dashboard_battery_left = lv_label_create(screen);
    dashboard_battery_dial = lv_label_create(screen);
    dashboard_battery_right = lv_label_create(screen);
    dashboard_layer_accent = lv_obj_create(screen);
    dashboard_layer = lv_label_create(screen);
    dashboard_mod_ctrl = lv_label_create(screen);
    dashboard_mod_alt = lv_label_create(screen);
    dashboard_mod_shift = lv_label_create(screen);
    dashboard_rule = lv_obj_create(screen);
    pet_image = lv_image_create(screen);
    media_badge = lv_obj_create(screen);
    media_play = lv_line_create(screen);
    media_pause_left = lv_obj_create(screen);
    media_pause_right = lv_obj_create(screen);
    if (dashboard_battery_left == NULL || dashboard_battery_dial == NULL ||
        dashboard_battery_right == NULL || dashboard_layer_accent == NULL ||
        dashboard_layer == NULL || dashboard_mod_ctrl == NULL || dashboard_mod_alt == NULL ||
        dashboard_mod_shift == NULL || dashboard_rule == NULL || pet_image == NULL ||
        media_badge == NULL || media_play == NULL || media_pause_left == NULL ||
        media_pause_right == NULL) {
        lv_obj_del(screen);
        return NULL;
    }

    lv_obj_add_style(dashboard_battery_left, &battery_style, LV_PART_MAIN);
    lv_obj_add_style(dashboard_battery_dial, &battery_style, LV_PART_MAIN);
    lv_obj_add_style(dashboard_battery_right, &battery_style, LV_PART_MAIN);
    lv_obj_add_style(dashboard_layer_accent, &layer_accent_style, LV_PART_MAIN);
    lv_obj_add_style(dashboard_layer, &layer_style, LV_PART_MAIN);
    lv_obj_add_style(dashboard_mod_ctrl, &modifier_style, LV_PART_MAIN);
    lv_obj_add_style(dashboard_mod_alt, &modifier_style, LV_PART_MAIN);
    lv_obj_add_style(dashboard_mod_shift, &modifier_style, LV_PART_MAIN);
    lv_obj_add_style(dashboard_rule, &rule_style, LV_PART_MAIN);
    lv_obj_add_style(media_badge, &media_badge_style, LV_PART_MAIN);
    lv_obj_add_style(media_play, &media_play_style, LV_PART_MAIN);
    lv_obj_add_style(media_pause_left, &media_pause_style, LV_PART_MAIN);
    lv_obj_add_style(media_pause_right, &media_pause_style, LV_PART_MAIN);

    lv_obj_set_size(dashboard_layer_accent, 4, 62);
    lv_obj_align(dashboard_layer_accent, LV_ALIGN_CENTER, -102, -5);
    lv_label_set_text(dashboard_mod_ctrl, "CTRL");
    lv_obj_align(dashboard_mod_ctrl, LV_ALIGN_BOTTOM_MID, -76, -18);
    lv_label_set_text(dashboard_mod_alt, "ALT");
    lv_obj_align(dashboard_mod_alt, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_label_set_text(dashboard_mod_shift, "SHIFT");
    lv_obj_align(dashboard_mod_shift, LV_ALIGN_BOTTOM_MID, 76, -18);
    lv_obj_set_size(dashboard_rule, 200, 1);
    lv_obj_align(dashboard_rule, LV_ALIGN_TOP_MID, 0, 49);

    lv_image_set_src(pet_image, &pet_image_descriptor);
    lv_obj_align(pet_image, LV_ALIGN_CENTER, ui_config.pet_x, ui_config.pet_y);
    set_hidden(pet_image, true);
    lv_obj_set_size(media_badge, 46, 46);
    lv_obj_align(media_badge, LV_ALIGN_CENTER, ui_config.pet_x + 70, ui_config.pet_y + 71);
    lv_line_set_points(media_play, media_play_points, ARRAY_SIZE(media_play_points));
    lv_obj_align(media_play, LV_ALIGN_CENTER, ui_config.pet_x + 71, ui_config.pet_y + 71);
    lv_obj_set_size(media_pause_left, 5, 20);
    lv_obj_align(media_pause_left, LV_ALIGN_CENTER, ui_config.pet_x + 64, ui_config.pet_y + 71);
    lv_obj_set_size(media_pause_right, 5, 20);
    lv_obj_align(media_pause_right, LV_ALIGN_CENTER, ui_config.pet_x + 76, ui_config.pet_y + 71);
    set_media_overlay_visible(false);

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
