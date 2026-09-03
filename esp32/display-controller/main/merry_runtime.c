/* SPDX-License-Identifier: MIT */

#include "merry_runtime.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "generated_layer_labels.h"

#define MERRY_PACK_MAGIC 0x3546504du /* MPF5 */
#define MERRY_PACK_VERSION 1u
#define MERRY_ANIMATION_COUNT 5u
#define MERRY_PET_SLOT_MAGIC 0x3153504du /* MPS1 */
#define MERRY_PET_SLOT_VERSION 1u
#define MERRY_PET_DATA_OFFSET 4096u
#define MERRY_PET_ID_BYTES 32u
#define MERRY_DEFAULT_SCREEN_OFF_DELAY_MS 300000u
#define MERRY_DEFAULT_MEDIA_TTL_MS 12000u
#define MERRY_KEYBOARD_ACTIVE_MS 20000u

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

struct merry_pet_slot_header {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t generation;
    uint32_t pack_size;
    uint32_t pack_crc32;
    char pet_id[MERRY_PET_ID_BYTES];
    uint32_t reserved[2];
    uint32_t header_crc32;
} __attribute__((packed));

_Static_assert(sizeof(struct merry_pack_header) == 64, "Merry pack header changed");
_Static_assert(sizeof(struct merry_animation_desc) == 8, "Merry animation descriptor changed");
_Static_assert(sizeof(struct merry_frame_desc) == 8, "Merry frame descriptor changed");
_Static_assert(sizeof(struct merry_pet_slot_header) == 64, "Merry pet slot header changed");

struct merry_context {
    SemaphoreHandle_t lock;
    TaskHandle_t renderer;
    const uint8_t *pack;
    size_t pack_size;
    const esp_partition_t *pet_slots[2];
    esp_partition_mmap_handle_t pack_map;
    uint8_t active_pet_slot;
    uint32_t pet_generation;
    char pet_id[MERRY_PET_ID_BYTES];
    bool pet_upload_active;
    uint8_t pet_upload_slot;
    size_t pet_upload_size;
    size_t pet_upload_written;
    uint32_t pet_upload_crc32;
    char pet_upload_id[MERRY_PET_ID_BYTES];
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
    uint32_t keyboard_activity_counter;
    uint32_t keyboard_generation;
    uint8_t keyboard_layer;
    uint8_t keyboard_modifiers;
    uint8_t keyboard_batteries[3];
    char keyboard_layer_name[10];
    bool keyboard_seen;
    int64_t keyboard_active_until;
    uint32_t screen_off_delay_ms;
    uint8_t display_mode;
    uint8_t brightness;
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

static bool validate_pack_bytes(const uint8_t *pack, size_t pack_size) {
    if (pack == NULL || pack_size < sizeof(struct merry_pack_header)) {
        return false;
    }
    const struct merry_pack_header *header = (const void *)pack;
    const uint32_t frame_bytes = (uint32_t)header->width * header->height * 2u;
    if (header->magic != MERRY_PACK_MAGIC || header->version != MERRY_PACK_VERSION ||
        header->header_size != sizeof(*header) || header->width > MERRY_LCD_WIDTH ||
        header->height > MERRY_LCD_HEIGHT || header->frame_count == 0 ||
        header->animation_count != MERRY_ANIMATION_COUNT ||
        header->total_size != pack_size ||
        !range_valid(header->animation_offset,
                     header->animation_count * sizeof(struct merry_animation_desc),
                     header->total_size) ||
        !range_valid(header->frame_offset,
                     header->frame_count * sizeof(struct merry_frame_desc),
                     header->total_size) ||
        header->data_offset > header->total_size ||
        merry_crc32(pack + header->header_size,
                    header->total_size - header->header_size) != header->content_crc) {
        return false;
    }
    const struct merry_animation_desc *animations =
        (const void *)(pack + header->animation_offset);
    const struct merry_frame_desc *frames = (const void *)(pack + header->frame_offset);
    for (uint16_t index = 0; index < header->animation_count; ++index) {
        const struct merry_animation_desc *animation = &animations[index];
        if (animation->id != index || animation->frame_count == 0 ||
            animation->first_frame > header->frame_count ||
            animation->frame_count > header->frame_count - animation->first_frame ||
            animation->reserved != 0) {
            return false;
        }
    }
    for (uint16_t index = 0; index < header->frame_count; ++index) {
        const struct merry_frame_desc *frame = &frames[index];
        if (frame->duration_ms < 20 || frame->reserved != 0 ||
            !range_valid(frame->data_offset, frame_bytes, header->total_size)) {
            return false;
        }
    }
    return true;
}

static bool valid_slot_header(const struct merry_pet_slot_header *header,
                              const esp_partition_t *partition) {
    return header->magic == MERRY_PET_SLOT_MAGIC &&
           header->version == MERRY_PET_SLOT_VERSION &&
           header->header_size == sizeof(*header) && header->pack_size > 0 &&
           header->pack_size <= partition->size - MERRY_PET_DATA_OFFSET &&
           header->pet_id[0] != '\0' && header->pet_id[MERRY_PET_ID_BYTES - 1] == '\0' &&
           header->reserved[0] == 0 && header->reserved[1] == 0 &&
           merry_crc32(header, offsetof(struct merry_pet_slot_header, header_crc32)) ==
               header->header_crc32;
}

static bool generation_newer(uint32_t left, uint32_t right) {
    return (int32_t)(left - right) > 0;
}

static esp_err_t map_slot(uint8_t slot, const struct merry_pet_slot_header *header,
                          const uint8_t **pack, esp_partition_mmap_handle_t *map) {
    const void *mapped = NULL;
    esp_err_t err = esp_partition_mmap(runtime.pet_slots[slot], MERRY_PET_DATA_OFFSET,
                                       header->pack_size, ESP_PARTITION_MMAP_DATA,
                                       &mapped, map);
    if (err != ESP_OK) return err;
    if (merry_crc32(mapped, header->pack_size) != header->pack_crc32 ||
        !validate_pack_bytes(mapped, header->pack_size)) {
        esp_partition_munmap(*map);
        return ESP_ERR_INVALID_CRC;
    }
    *pack = mapped;
    return ESP_OK;
}

static esp_err_t load_pack(void) {
    runtime.pet_slots[0] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "pet_a");
    runtime.pet_slots[1] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, "pet_b");
    if (runtime.pet_slots[0] == NULL || runtime.pet_slots[1] == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    bool found = false;
    struct merry_pet_slot_header chosen = {0};
    uint8_t chosen_slot = 0;
    for (uint8_t slot = 0; slot < 2; ++slot) {
        struct merry_pet_slot_header header = {0};
        if (esp_partition_read(runtime.pet_slots[slot], 0, &header, sizeof(header)) != ESP_OK ||
            !valid_slot_header(&header, runtime.pet_slots[slot])) continue;
        const uint8_t *candidate = NULL;
        esp_partition_mmap_handle_t candidate_map = 0;
        if (map_slot(slot, &header, &candidate, &candidate_map) != ESP_OK) continue;
        esp_partition_munmap(candidate_map);
        if (!found || generation_newer(header.generation, chosen.generation)) {
            found = true;
            chosen = header;
            chosen_slot = slot;
        }
    }
    if (!found) return ESP_ERR_INVALID_CRC;

    esp_err_t err = map_slot(chosen_slot, &chosen, &runtime.pack, &runtime.pack_map);
    if (err != ESP_OK) return err;
    runtime.pack_size = chosen.pack_size;
    runtime.active_pet_slot = chosen_slot;
    runtime.pet_generation = chosen.generation;
    memcpy(runtime.pet_id, chosen.pet_id, sizeof(runtime.pet_id));
    runtime.header = (const void *)runtime.pack;
    runtime.animations = (const void *)(runtime.pack + runtime.header->animation_offset);
    runtime.frames = (const void *)(runtime.pack + runtime.header->frame_offset);
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
    runtime.screen_off_delay_ms = MERRY_DEFAULT_SCREEN_OFF_DELAY_MS;
    runtime.display_mode = MERRY_MODE_AUTO;
    runtime.brightness = 100u;
    snprintf(runtime.keyboard_layer_name, sizeof(runtime.keyboard_layer_name), "BASE");
    runtime.screen_off_at = monotonic_ms() + runtime.screen_off_delay_ms;
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
        runtime.screen_off_at = now + runtime.screen_off_delay_ms;
    }
}

static bool display_should_be_on_locked(int64_t now) {
    if (runtime.host_state == MERRY_HOST_DISPLAY_OFF) {
        return false;
    }
    if (runtime.display_mode == MERRY_MODE_PET ||
        runtime.display_mode == MERRY_MODE_DASHBOARD) {
        return true;
    }
    if (now < runtime.keyboard_active_until || runtime.codex_state != MERRY_ANIM_IDLE ||
        runtime.media_state == MERRY_MEDIA_PLAYING) {
        return true;
    }
    return runtime.host_state == MERRY_HOST_ACTIVE || now < runtime.screen_off_at;
}

void merry_runtime_set_keyboard(uint32_t activity_counter, uint8_t layer,
                                uint8_t modifiers, uint8_t battery_left,
                                uint8_t battery_dial, uint8_t battery_right,
                                const char *layer_name) {
    const int64_t now = monotonic_ms();
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    const bool activity_changed = !runtime.keyboard_seen ||
                                  activity_counter != runtime.keyboard_activity_counter;
    runtime.keyboard_seen = true;
    runtime.keyboard_activity_counter = activity_counter;
    runtime.keyboard_layer = layer;
    runtime.keyboard_modifiers = modifiers;
    runtime.keyboard_batteries[0] = battery_left;
    runtime.keyboard_batteries[1] = battery_dial;
    runtime.keyboard_batteries[2] = battery_right;
    snprintf(runtime.keyboard_layer_name, sizeof(runtime.keyboard_layer_name), "%s",
             layer_name != NULL && layer_name[0] != '\0' ? layer_name : "BASE");
    if (activity_changed) {
        runtime.keyboard_active_until = now + MERRY_KEYBOARD_ACTIVE_MS;
        runtime.screen_off_at = now + runtime.screen_off_delay_ms;
    }
    ++runtime.keyboard_generation;
    xSemaphoreGive(runtime.lock);
    wake_renderer();
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
        runtime.screen_off_at = now + runtime.screen_off_delay_ms;
    } else if (state == MERRY_HOST_AFK && previous != MERRY_HOST_AFK) {
        runtime.screen_off_at = now + runtime.screen_off_delay_ms;
    }
    xSemaphoreGive(runtime.lock);
    wake_renderer();
    return true;
}

bool merry_runtime_set_config(uint8_t mode, uint8_t brightness,
                              uint32_t screen_off_ms) {
    if (mode > MERRY_MODE_DASHBOARD || brightness < 10u || brightness > 100u ||
        screen_off_ms < 30000u || screen_off_ms > 3600000u) {
        return false;
    }
    const int64_t now = monotonic_ms();
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    runtime.display_mode = mode;
    runtime.brightness = brightness;
    runtime.screen_off_delay_ms = screen_off_ms;
    runtime.screen_off_at = now + screen_off_ms;
    xSemaphoreGive(runtime.lock);
    wake_renderer();
    return true;
}

uint8_t merry_runtime_display_mode(void) {
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    const uint8_t mode = runtime.display_mode;
    xSemaphoreGive(runtime.lock);
    return mode;
}

uint8_t merry_runtime_brightness(void) {
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    const uint8_t brightness = runtime.brightness;
    xSemaphoreGive(runtime.lock);
    return brightness;
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

bool merry_runtime_pet_info(char pet_id[MERRY_PET_ID_BYTES], uint32_t *generation,
                            uint32_t *maximum_size, uint8_t *active_slot) {
    if (pet_id == NULL || generation == NULL || maximum_size == NULL ||
        active_slot == NULL) return false;
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    memcpy(pet_id, runtime.pet_id, MERRY_PET_ID_BYTES);
    *generation = runtime.pet_generation;
    *maximum_size = runtime.pet_slots[0]->size - MERRY_PET_DATA_OFFSET;
    *active_slot = runtime.active_pet_slot;
    xSemaphoreGive(runtime.lock);
    return true;
}

bool merry_runtime_pet_upload_begin(const char *pet_id, size_t pack_size,
                                    uint32_t pack_crc32) {
    if (pet_id == NULL || pet_id[0] == '\0' ||
        strnlen(pet_id, MERRY_PET_ID_BYTES) >= MERRY_PET_ID_BYTES ||
        pack_size == 0 || runtime.pet_slots[0] == NULL ||
        pack_size > runtime.pet_slots[0]->size - MERRY_PET_DATA_OFFSET) return false;

    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    if (runtime.pet_upload_active) {
        xSemaphoreGive(runtime.lock);
        return false;
    }
    runtime.pet_upload_active = true;
    runtime.pet_upload_slot = runtime.active_pet_slot ^ 1u;
    runtime.pet_upload_size = pack_size;
    runtime.pet_upload_written = 0;
    runtime.pet_upload_crc32 = pack_crc32;
    snprintf(runtime.pet_upload_id, sizeof(runtime.pet_upload_id), "%s", pet_id);
    const esp_partition_t *partition = runtime.pet_slots[runtime.pet_upload_slot];
    xSemaphoreGive(runtime.lock);

    if (esp_partition_erase_range(partition, 0, partition->size) != ESP_OK) {
        merry_runtime_pet_upload_abort();
        return false;
    }
    return true;
}

bool merry_runtime_pet_upload_write(size_t offset, const uint8_t *data, size_t size) {
    if (data == NULL || size == 0) return false;
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    const bool valid = runtime.pet_upload_active && offset == runtime.pet_upload_written &&
                       offset <= runtime.pet_upload_size &&
                       size <= runtime.pet_upload_size - offset;
    const esp_partition_t *partition = valid ? runtime.pet_slots[runtime.pet_upload_slot] : NULL;
    xSemaphoreGive(runtime.lock);
    if (!valid || esp_partition_write(partition, MERRY_PET_DATA_OFFSET + offset,
                                      data, size) != ESP_OK) return false;
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    runtime.pet_upload_written += size;
    xSemaphoreGive(runtime.lock);
    return true;
}

bool merry_runtime_pet_upload_finish(void) {
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    const bool complete = runtime.pet_upload_active &&
                          runtime.pet_upload_written == runtime.pet_upload_size;
    const uint8_t slot = runtime.pet_upload_slot;
    const size_t pack_size = runtime.pet_upload_size;
    const uint32_t pack_crc32 = runtime.pet_upload_crc32;
    const uint32_t generation = runtime.pet_generation + 1u;
    char pet_id[MERRY_PET_ID_BYTES];
    memcpy(pet_id, runtime.pet_upload_id, sizeof(pet_id));
    xSemaphoreGive(runtime.lock);
    if (!complete) {
        merry_runtime_pet_upload_abort();
        return false;
    }

    struct merry_pet_slot_header header = {
        .magic = MERRY_PET_SLOT_MAGIC,
        .version = MERRY_PET_SLOT_VERSION,
        .header_size = sizeof(header),
        .generation = generation,
        .pack_size = pack_size,
        .pack_crc32 = pack_crc32,
    };
    memcpy(header.pet_id, pet_id, sizeof(header.pet_id));

    const uint8_t *mapped = NULL;
    esp_partition_mmap_handle_t map = 0;
    if (map_slot(slot, &header, &mapped, &map) != ESP_OK) {
        merry_runtime_pet_upload_abort();
        return false;
    }
    esp_partition_munmap(map);
    header.header_crc32 = merry_crc32(
        &header, offsetof(struct merry_pet_slot_header, header_crc32));
    if (esp_partition_write(runtime.pet_slots[slot], 0, &header, sizeof(header)) != ESP_OK) {
        merry_runtime_pet_upload_abort();
        return false;
    }
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    runtime.pet_upload_active = false;
    xSemaphoreGive(runtime.lock);
    return true;
}

void merry_runtime_pet_upload_abort(void) {
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    runtime.pet_upload_active = false;
    runtime.pet_upload_size = 0;
    runtime.pet_upload_written = 0;
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

static const uint8_t font_digits[10][5] = {
    {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0x00, 0x42, 0x7f, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4b, 0x31},
    {0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3c, 0x4a, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1e},
};

static const uint8_t font_letters[26][5] = {
    {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},
    {0x3e,0x41,0x41,0x41,0x22},{0x7f,0x41,0x41,0x22,0x1c},
    {0x7f,0x49,0x49,0x49,0x41},{0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x7a},{0x7f,0x08,0x08,0x08,0x7f},
    {0x00,0x41,0x7f,0x41,0x00},{0x20,0x40,0x41,0x3f,0x01},
    {0x7f,0x08,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f},{0x7f,0x04,0x08,0x10,0x7f},
    {0x3e,0x41,0x41,0x41,0x3e},{0x7f,0x09,0x09,0x09,0x06},
    {0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7f,0x01,0x01},
    {0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},
    {0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
};

static const uint8_t *glyph_for(char value) {
    static const uint8_t blank[5] = {0};
    static const uint8_t dash[5] = {0x08,0x08,0x08,0x08,0x08};
    if (value >= '0' && value <= '9') return font_digits[value - '0'];
    if (value >= 'a' && value <= 'z') value = (char)(value - 'a' + 'A');
    if (value >= 'A' && value <= 'Z') return font_letters[value - 'A'];
    if (value == '-') return dash;
    return blank;
}

static void draw_char(uint16_t *frame, int x, int y, char value, int scale,
                      uint16_t colour) {
    const uint8_t *glyph = glyph_for(value);
    for (int column = 0; column < 5; ++column) {
        for (int row = 0; row < 7; ++row) {
            if ((glyph[column] & (1u << row)) == 0) continue;
            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    put_pixel(frame, x + column * scale + dx,
                              y + row * scale + dy, colour);
                }
            }
        }
    }
}

static int text_width(const char *text, int scale) {
    return text == NULL ? 0 : (int)strlen(text) * 6 * scale - scale;
}

static void draw_text(uint16_t *frame, int x, int y, const char *text, int scale,
                      uint16_t colour) {
    for (; text != NULL && *text != '\0'; ++text, x += 6 * scale) {
        draw_char(frame, x, y, *text, scale, colour);
    }
}

static uint16_t battery_colour(uint8_t level) {
    if (level == 0) return rgb565(95, 89, 100);
    if (level <= 20) return rgb565(255, 69, 58);
    if (level <= 50) return rgb565(255, 159, 10);
    return rgb565(50, 215, 75);
}

static bool draw_antialiased_layer(uint16_t *frame, const char *name,
                                   uint8_t red, uint8_t green, uint8_t blue) {
    for (size_t index = 0; index < sizeof(merry_layer_labels) / sizeof(merry_layer_labels[0]);
         ++index) {
        const struct merry_layer_label *label = &merry_layer_labels[index];
        if (strcmp(label->name, name) != 0) {
            continue;
        }
        const int origin_x = (MERRY_LCD_WIDTH - label->width) / 2;
        const int origin_y = 89 + (34 - label->height) / 2;
        for (uint16_t y = 0; y < label->height; ++y) {
            for (uint16_t x = 0; x < label->width; ++x) {
                const uint8_t alpha = label->alpha[y * label->width + x];
                if (alpha == 0) {
                    continue;
                }
                put_pixel(frame, origin_x + x, origin_y + y,
                          rgb565((uint8_t)((red * alpha + 127u) / 255u),
                                 (uint8_t)((green * alpha + 127u) / 255u),
                                 (uint8_t)((blue * alpha + 127u) / 255u)));
            }
        }
        return true;
    }
    return false;
}

static void render_dashboard(uint16_t *destination) {
    struct {
        uint8_t modifiers;
        uint8_t batteries[3];
        char layer_name[10];
    } state;
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    state.modifiers = runtime.keyboard_modifiers;
    memcpy(state.batteries, runtime.keyboard_batteries, sizeof(state.batteries));
    memcpy(state.layer_name, runtime.keyboard_layer_name, sizeof(state.layer_name));
    xSemaphoreGive(runtime.lock);

    memset(destination, 0, MERRY_LCD_FRAME_BYTES);
    const uint16_t purple = rgb565(191, 90, 242);
    const uint16_t dim = rgb565(73, 67, 79);
    const uint16_t rule = rgb565(44, 40, 48);
    const char labels[3] = {'L', 'D', 'R'};
    const int centers[3] = {42, 120, 198};
    for (int index = 0; index < 3; ++index) {
        char battery[8];
        if (state.batteries[index] == 0) {
            snprintf(battery, sizeof(battery), "%c--", labels[index]);
        } else {
            snprintf(battery, sizeof(battery), "%c%u", labels[index],
                     state.batteries[index]);
        }
        draw_text(destination, centers[index] - text_width(battery, 2) / 2, 15,
                  battery, 2, battery_colour(state.batteries[index]));
    }
    for (int x = 20; x < 220; ++x) put_pixel(destination, x, 48, rule);
    for (int y = 76; y < 138; ++y) {
        for (int x = 17; x < 21; ++x) put_pixel(destination, x, y, purple);
    }
    if (!draw_antialiased_layer(destination, state.layer_name, 191, 90, 242)) {
        const int layer_scale = strlen(state.layer_name) <= 5 ? 5 : 4;
        draw_text(destination, (240 - text_width(state.layer_name, layer_scale)) / 2,
                  88, state.layer_name, layer_scale, purple);
    }
    for (int x = 20; x < 220; ++x) put_pixel(destination, x, 176, rule);
    const char *mods[3] = {"CTRL", "ALT", "SHIFT"};
    const uint8_t masks[3] = {0x11, 0x44, 0x22};
    for (int index = 0; index < 3; ++index) {
        draw_text(destination, centers[index] - text_width(mods[index], 2) / 2, 199,
                  mods[index], 2,
                  (state.modifiers & masks[index]) != 0 ? purple : dim);
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
            const int half_height = ((16 - x) * 11 + 8) / 16;
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
    enum { RENDER_NONE, RENDER_PET, RENDER_MEDIA, RENDER_DASHBOARD };
    static int previous_mode = RENDER_NONE;
    static uint8_t previous_animation = 0xff;
    static uint8_t previous_media_state = 0xff;
    static uint32_t previous_media_generation = UINT32_MAX;
    static uint32_t previous_keyboard_generation = UINT32_MAX;
    static uint16_t animation_frame;
    static int64_t frame_deadline;
    static bool previous_screen_on;

    const int64_t now = monotonic_ms();
    xSemaphoreTake(runtime.lock, portMAX_DELAY);
    expire_states_locked(now);
    const uint8_t codex = runtime.codex_state;
    const uint8_t media = runtime.media_state;
    const uint8_t display_mode = runtime.display_mode;
    bool keyboard_mode = runtime.keyboard_seen && now < runtime.keyboard_active_until;
    bool media_mode = runtime.media_valid && codex != MERRY_ANIM_RUNNING &&
                      (media == MERRY_MEDIA_PLAYING || media == MERRY_MEDIA_PAUSED);
    uint8_t selected_codex = codex;
    if (display_mode == MERRY_MODE_SPOTIFY) {
        keyboard_mode = false;
        selected_codex = MERRY_ANIM_IDLE;
    } else if (display_mode == MERRY_MODE_CODEX) {
        keyboard_mode = false;
        media_mode = false;
    } else if (display_mode == MERRY_MODE_PET) {
        keyboard_mode = false;
        media_mode = false;
        selected_codex = MERRY_ANIM_IDLE;
    } else if (display_mode == MERRY_MODE_DASHBOARD) {
        keyboard_mode = true;
        media_mode = false;
        selected_codex = MERRY_ANIM_IDLE;
    }
    const uint32_t generation = runtime.media_generation;
    const uint32_t keyboard_generation = runtime.keyboard_generation;
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
    if (keyboard_mode) {
        if (previous_mode != RENDER_DASHBOARD ||
            previous_keyboard_generation != keyboard_generation) {
            render_dashboard(destination);
            *changed = true;
            previous_mode = RENDER_DASHBOARD;
            previous_keyboard_generation = keyboard_generation;
        }
        return 50;
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

    const struct merry_animation_desc *animation = &runtime.animations[selected_codex];
    if (previous_mode != RENDER_PET || previous_animation != selected_codex) {
        animation_frame = 0;
        frame_deadline = now;
        previous_mode = RENDER_PET;
        previous_animation = selected_codex;
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
