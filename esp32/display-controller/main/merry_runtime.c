/* SPDX-License-Identifier: MIT */

#include "merry_runtime.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

#define MERRY_PACK_MAGIC 0x3546504du /* MPF5 */
#define MERRY_PACK_VERSION 1u
#define MERRY_PACK_PATH "/assets/merry-full.petpack"
#define MERRY_ANIMATION_COUNT 5u
#define MERRY_SCREEN_OFF_DELAY_MS 300000u
#define MERRY_DEFAULT_MEDIA_TTL_MS 12000u

struct merry_pack_header {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint16_t width;
    uint16_t height;
    uint16_t frame_count;
    uint16_t animation_count;
    uint32_t animation_offset;
    uint32_t frame_offset;
    uint32_t data_offset;
    uint32_t total_size;
    uint32_t content_crc;
    uint32_t reserved[7];
} __attribute__((packed));

struct merry_animation_desc {
    uint8_t id;
    uint8_t flags;
    uint16_t first_frame;
    uint16_t frame_count;
    uint16_t reserved;
} __attribute__((packed));

struct merry_frame_desc {
    uint32_t data_offset;
    uint16_t duration_ms;
    uint16_t reserved;
} __attribute__((packed));

_Static_assert(sizeof(struct merry_pack_header) == 64, "Merry pack header changed");
_Static_assert(sizeof(struct merry_animation_desc) == 8, "Merry animation descriptor changed");
_Static_assert(sizeof(struct merry_frame_desc) == 8, "Merry frame descriptor changed");

struct merry_context {
    SemaphoreHandle_t lock;
    TaskHandle_t renderer;
    uint8_t *pack;
    size_t pack_size;
    const struct merry_pack_header *header;
    const struct merry_animation_desc *animations;
    const struct merry_frame_desc *frames;
    uint16_t *media[2];
    uint8_t active_media;
    uint8_t staging_media;
    bool media_valid;
    bool upload_active;
    uint32_t media_generation;
    uint8_t codex_state;
    uint8_t media_state;
    uint8_t host_state;
    int64_t codex_expires_at;
    int64_t media_expires_at;
    int64_t host_expires_at;
    int64_t screen_off_at;
};

static struct merry_context runtime;

static int64_t monotonic_ms(void) {
    return esp_timer_get_time() / 1000;
}

uint32_t merry_crc32(const void *data, size_t size) {
    const uint8_t *bytes = data;
    uint32_t crc = 0xffffffffu;
    for (size_t index = 0; index < size; ++index) {
        crc ^= bytes[index];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
        }
    }
    return crc ^ 0xffffffffu;
}

static void wake_renderer(void) {
    TaskHandle_t renderer = runtime.renderer;
    if (renderer != NULL) {
        xTaskNotifyGive(renderer);
    }
}

static bool range_valid(uint32_t offset, uint32_t size, uint32_t total) {
    return offset <= total && size <= total - offset;
}

static bool validate_pack(void) {
    if (runtime.pack_size < sizeof(struct merry_pack_header)) {
        return false;
    }
    const struct merry_pack_header *header = (const void *)runtime.pack;
    const uint32_t frame_bytes = (uint32_t)header->width * header->height * 2u;
    if (header->magic != MERRY_PACK_MAGIC || header->version != MERRY_PACK_VERSION ||
        header->header_size != sizeof(*header) || header->width > MERRY_LCD_WIDTH ||
        header->height > MERRY_LCD_HEIGHT || header->frame_count == 0 ||
        header->animation_count != MERRY_ANIMATION_COUNT ||
        header->total_size != runtime.pack_size ||
        !range_valid(header->animation_offset,
                     header->animation_count * sizeof(struct merry_animation_desc),
                     header->total_size) ||
        !range_valid(header->frame_offset,
                     header->frame_count * sizeof(struct merry_frame_desc),
                     header->total_size) ||
        header->data_offset > header->total_size ||
        merry_crc32(runtime.pack + header->header_size,
                    header->total_size - header->header_size) != header->content_crc) {
        return false;
    }

    runtime.header = header;
    runtime.animations = (const void *)(runtime.pack + header->animation_offset);
    runtime.frames = (const void *)(runtime.pack + header->frame_offset);
    for (uint16_t index = 0; index < header->animation_count; ++index) {
        const struct merry_animation_desc *animation = &runtime.animations[index];
        if (animation->id != index || animation->frame_count == 0 ||
            animation->first_frame > header->frame_count ||
            animation->frame_count > header->frame_count - animation->first_frame ||
            animation->reserved != 0) {
            return false;
        }
    }
    for (uint16_t index = 0; index < header->frame_count; ++index) {
        const struct merry_frame_desc *frame = &runtime.frames[index];
        if (frame->duration_ms < 20 || frame->reserved != 0 ||
            !range_valid(frame->data_offset, frame_bytes, header->total_size)) {
            return false;
        }
    }
    return true;
}

static esp_err_t load_pack(void) {
    const esp_vfs_spiffs_conf_t storage = {
        .base_path = "/assets",
        .partition_label = "assets",
        .max_files = 2,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&storage);
    if (err != ESP_OK) {
        return err;
    }

    FILE *file = fopen(MERRY_PACK_PATH, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        return ESP_ERR_NOT_FOUND;
    }
    const long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    runtime.pack_size = (size_t)length;
    runtime.pack = heap_caps_aligned_alloc(64, runtime.pack_size,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (runtime.pack == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    const size_t read = fread(runtime.pack, 1, runtime.pack_size, file);
    fclose(file);
    if (read != runtime.pack_size || !validate_pack()) {
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

esp_err_t merry_runtime_init(void) {
    memset(&runtime, 0, sizeof(runtime));
    runtime.lock = xSemaphoreCreateMutex();
    if (runtime.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    runtime.media[0] = heap_caps_aligned_alloc(64, MERRY_MEDIA_BYTES,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    runtime.media[1] = heap_caps_aligned_alloc(64, MERRY_MEDIA_BYTES,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (runtime.media[0] == NULL || runtime.media[1] == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = load_pack();
    if (err != ESP_OK) {
        return err;
    }
    runtime.codex_state = MERRY_ANIM_IDLE;
    runtime.media_state = MERRY_MEDIA_NONE;
    runtime.host_state = MERRY_HOST_AFK;
    runtime.screen_off_at = monotonic_ms() + MERRY_SCREEN_OFF_DELAY_MS;
    return ESP_OK;
}

void merry_runtime_attach_renderer(TaskHandle_t renderer) {
    runtime.renderer = renderer;
}

static void expire_states_locked(int64_t now) {
    if (runtime.codex_expires_at > 0 && now >= runtime.codex_expires_at) {
        runtime.codex_state = MERRY_ANIM_IDLE;
        runtime.codex_expires_at = 0;
    }
    if (runtime.media_expires_at > 0 && now >= runtime.media_expires_at) {
        runtime.media_state = MERRY_MEDIA_NONE;
        runtime.media_expires_at = 0;
    }
    if (runtime.host_expires_at > 0 && now >= runtime.host_expires_at) {
        runtime.host_state = MERRY_HOST_AFK;
        runtime.host_expires_at = 0;
        runtime.screen_off_at = now + MERRY_SCREEN_OFF_DELAY_MS;
    }
}

static bool display_should_be_on_locked(int64_t now) {
    if (runtime.host_state == MERRY_HOST_DISPLAY_OFF) {
        return false;
    }
    if (runtime.codex_state != MERRY_ANIM_IDLE || runtime.media_state == MERRY_MEDIA_PLAYING) {
        return true;
    }
    return runtime.host_state == MERRY_HOST_ACTIVE || now < runtime.screen_off_at;
}

uint8_t merry_runtime_codex_state(void) {
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    uint8_t state = runtime.codex_state;
    xSemaphoreGive(runtime.lock);
    return state;
}

bool merry_runtime_set_codex(uint8_t state, uint32_t ttl_ms) {
    if (state > MERRY_ANIM_BLOCKED) {
        return false;
    }
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    runtime.codex_state = state;
    runtime.codex_expires_at = monotonic_ms() + ttl_ms;
    xSemaphoreGive(runtime.lock);
    wake_renderer();
    return true;
}

uint8_t merry_runtime_media_state(void) {
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    uint8_t state = runtime.media_state;
    xSemaphoreGive(runtime.lock);
    return state;
}

bool merry_runtime_set_media(uint8_t state, uint32_t ttl_ms) {
    if (state > MERRY_MEDIA_PAUSED) {
        return false;
    }
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    runtime.media_state = state;
    runtime.media_expires_at = monotonic_ms() + ttl_ms;
    xSemaphoreGive(runtime.lock);
    wake_renderer();
    return true;
}

uint8_t merry_runtime_host_state(void) {
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    uint8_t state = runtime.host_state;
    xSemaphoreGive(runtime.lock);
    return state;
}

bool merry_runtime_set_host(uint8_t state, uint32_t ttl_ms) {
    if (state > MERRY_HOST_DISPLAY_OFF) {
        return false;
    }
    const int64_t now = monotonic_ms();
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    const uint8_t previous = runtime.host_state;
    runtime.host_state = state;
    runtime.host_expires_at = state == MERRY_HOST_AFK ? 0 : now + ttl_ms;
    if (state == MERRY_HOST_ACTIVE) {
        runtime.screen_off_at = now + MERRY_SCREEN_OFF_DELAY_MS;
    } else if (state == MERRY_HOST_AFK && previous != MERRY_HOST_AFK) {
        runtime.screen_off_at = now + MERRY_SCREEN_OFF_DELAY_MS;
    }
    xSemaphoreGive(runtime.lock);
    wake_renderer();
    return true;
}

bool merry_runtime_media_upload_begin(void) {
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    if (runtime.upload_active) {
        xSemaphoreGive(runtime.lock);
        return false;
    }
    runtime.staging_media = runtime.active_media ^ 1u;
    runtime.upload_active = true;
    xSemaphoreGive(runtime.lock);
    return true;
}

bool merry_runtime_media_upload_write(size_t offset, const uint8_t *data, size_t size) {
    if (data == NULL || offset > MERRY_MEDIA_BYTES || size > MERRY_MEDIA_BYTES - offset) {
        return false;
    }
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    const bool active = runtime.upload_active;
    uint16_t *staging = runtime.media[runtime.staging_media];
    xSemaphoreGive(runtime.lock);
    if (!active) {
        return false;
    }
    memcpy((uint8_t *)staging + offset, data, size);
    return true;
}

bool merry_runtime_media_upload_finish(uint32_t expected_crc, uint8_t state) {
    if (state < MERRY_MEDIA_PLAYING || state > MERRY_MEDIA_PAUSED) {
        merry_runtime_media_upload_abort();
        return false;
    }
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    const bool active = runtime.upload_active;
    uint16_t *staging = runtime.media[runtime.staging_media];
    xSemaphoreGive(runtime.lock);
    if (!active || merry_crc32(staging, MERRY_MEDIA_BYTES) != expected_crc) {
        merry_runtime_media_upload_abort();
        return false;
    }

    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    runtime.active_media = runtime.staging_media;
    runtime.media_valid = true;
    runtime.upload_active = false;
    runtime.media_state = state;
    runtime.media_expires_at = monotonic_ms() + MERRY_DEFAULT_MEDIA_TTL_MS;
    ++runtime.media_generation;
    xSemaphoreGive(runtime.lock);
    wake_renderer();
    return true;
}

void merry_runtime_media_upload_abort(void) {
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    runtime.upload_active = false;
    xSemaphoreGive(runtime.lock);
}

static inline uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue) {
    return (uint16_t)(((uint16_t)(red & 0xf8u) << 8) |
                      ((uint16_t)(green & 0xfcu) << 3) | (blue >> 3));
}

static void put_pixel(uint16_t *frame, int x, int y, uint16_t colour) {
    if ((unsigned)x < MERRY_LCD_WIDTH && (unsigned)y < MERRY_LCD_HEIGHT) {
        frame[y * MERRY_LCD_WIDTH + x] = colour;
    }
}

static void draw_media_icon(uint16_t *frame, uint8_t state) {
    const int center_x = 190;
    const int center_y = 190;
    const int radius = 23;
    const uint16_t black = rgb565(0, 0, 0);
    const uint16_t white = rgb565(255, 255, 255);
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            const int distance = x * x + y * y;
            if (distance <= radius * radius) {
                put_pixel(frame, center_x + x, center_y + y,
                          distance >= (radius - 2) * (radius - 2) ? white : black);
            }
        }
    }
    if (state == MERRY_MEDIA_PLAYING) {
        for (int y = -10; y <= 10; ++y) {
            for (int x = -7; x <= -3; ++x) {
                put_pixel(frame, center_x + x, center_y + y, white);
            }
            for (int x = 3; x <= 7; ++x) {
                put_pixel(frame, center_x + x, center_y + y, white);
            }
        }
    } else {
        /* The centroid of a triangle sits one third of the way from its base.
         * Offset the geometry left so the perceived play icon is centered. */
        for (int x = 0; x <= 16; ++x) {
            const int half_height = (x * 11 + 8) / 16;
            for (int y = -half_height; y <= half_height; ++y) {
                put_pixel(frame, center_x - 6 + x, center_y + y, white);
            }
        }
    }
}

static void render_media(uint16_t *destination, uint8_t state) {
    memset(destination, 0, MERRY_LCD_FRAME_BYTES);
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    const uint16_t *source = runtime.media[runtime.active_media];
    for (unsigned y = 0; y < MERRY_MEDIA_HEIGHT; ++y) {
        memcpy(destination + (y + 10u) * MERRY_LCD_WIDTH + 19u,
               source + y * MERRY_MEDIA_WIDTH, MERRY_MEDIA_WIDTH * sizeof(uint16_t));
    }
    xSemaphoreGive(runtime.lock);
    draw_media_icon(destination, state);
}

static void render_pet(uint16_t *destination, uint16_t frame_index) {
    memset(destination, 0, MERRY_LCD_FRAME_BYTES);
    const struct merry_frame_desc *frame = &runtime.frames[frame_index];
    const uint16_t *source = (const void *)(runtime.pack + frame->data_offset);
    const unsigned x = (MERRY_LCD_WIDTH - runtime.header->width) / 2u;
    const unsigned y = (MERRY_LCD_HEIGHT - runtime.header->height) / 2u;
    for (unsigned row = 0; row < runtime.header->height; ++row) {
        memcpy(destination + (row + y) * MERRY_LCD_WIDTH + x,
               source + row * runtime.header->width,
               runtime.header->width * sizeof(uint16_t));
    }
}

uint32_t merry_runtime_render(uint16_t *destination, bool *changed, bool *screen_on) {
    enum { RENDER_NONE, RENDER_PET, RENDER_MEDIA };
    static int previous_mode = RENDER_NONE;
    static uint8_t previous_animation = 0xff;
    static uint8_t previous_media_state = 0xff;
    static uint32_t previous_media_generation = UINT32_MAX;
    static uint16_t animation_frame;
    static int64_t frame_deadline;
    static bool previous_screen_on;

    const int64_t now = monotonic_ms();
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    expire_states_locked(now);
    const uint8_t codex = runtime.codex_state;
    const uint8_t media = runtime.media_state;
    const bool media_mode = runtime.media_valid && codex != MERRY_ANIM_RUNNING &&
                            (media == MERRY_MEDIA_PLAYING || media == MERRY_MEDIA_PAUSED);
    const uint32_t generation = runtime.media_generation;
    const bool on = display_should_be_on_locked(now);
    xSemaphoreGive(runtime.lock);

    *changed = false;
    *screen_on = on;
    if (!on) {
        previous_mode = RENDER_NONE;
        previous_screen_on = false;
        return 100;
    }

    if (!previous_screen_on) {
        previous_mode = RENDER_NONE;
        previous_screen_on = true;
    }
    if (media_mode) {
        if (previous_mode != RENDER_MEDIA || previous_media_state != media ||
            previous_media_generation != generation) {
            render_media(destination, media);
            *changed = true;
            previous_mode = RENDER_MEDIA;
            previous_media_state = media;
            previous_media_generation = generation;
        }
        return 100;
    }

    const struct merry_animation_desc *animation = &runtime.animations[codex];
    if (previous_mode != RENDER_PET || previous_animation != codex) {
        animation_frame = 0;
        frame_deadline = now;
        previous_mode = RENDER_PET;
        previous_animation = codex;
    }
    if (now >= frame_deadline) {
        const uint16_t absolute_frame = animation->first_frame + animation_frame;
        render_pet(destination, absolute_frame);
        *changed = true;
        frame_deadline = now + runtime.frames[absolute_frame].duration_ms;
        if (animation_frame + 1u < animation->frame_count) {
            ++animation_frame;
        } else if ((animation->flags & 1u) != 0u) {
            animation_frame = 0;
        }
    }
    const int64_t remaining = frame_deadline - now;
    return remaining <= 1 ? 1u : remaining > 500 ? 500u : (uint32_t)remaining;
}
