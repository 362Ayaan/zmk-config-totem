/* SPDX-License-Identifier: MIT */

#include "pet_store.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "merry_fallback.h"

LOG_MODULE_REGISTER(merry_store, CONFIG_ZMK_LOG_LEVEL);

#define PET_FLASH_NODE DT_ALIAS(merry_flash)
#define PET_SLOT_MAGIC 0x3159524du /* MRY1 */
#define PET_SLOT_VERSION 1u
#define PET_SLOT_SIZE (1024u * 1024u)
#define PET_SLOT_HEADER_AREA 4096u
#define PET_SLOT_COUNT 2u
#define PET_SLOT_FLAG_COMMITTED 0x434f4d4du /* COMM */
#define PET_IO_CHUNK_SIZE 512u

BUILD_ASSERT(DT_NODE_HAS_STATUS(PET_FLASH_NODE, okay), "Merry QSPI flash alias is not ready");
BUILD_ASSERT(DT_PROP(PET_FLASH_NODE, size) >= (PET_SLOT_COUNT * PET_SLOT_SIZE * 8u),
             "Merry requires the XIAO's 2 MB QSPI flash");
BUILD_ASSERT(sizeof(struct pet_pack_header) == 40u, "pet pack header layout changed");
BUILD_ASSERT(sizeof(struct pet_animation_desc) == 8u, "animation descriptor layout changed");
BUILD_ASSERT(sizeof(struct pet_frame_desc) == 40u, "frame descriptor layout changed");
BUILD_ASSERT(PET_FRAME_PACKED_BYTES == MERRY_FALLBACK_DATA_SIZE,
             "fallback and decoder dimensions disagree");

struct pet_slot_header {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t generation;
    uint32_t pack_size;
    uint32_t pack_crc32;
    uint32_t flags;
    uint32_t reserved0;
    uint32_t reserved1;
} __packed;

BUILD_ASSERT(sizeof(struct pet_slot_header) == 32u, "slot header layout changed");

static const struct device *const pet_flash = DEVICE_DT_GET(PET_FLASH_NODE);
K_MUTEX_DEFINE(store_mutex);

static bool initialized;
static bool current_valid;
static uint8_t current_slot;
static uint32_t current_generation;
static uint32_t current_pack_size;
static struct pet_pack_header current_pack;

static bool upload_active;
static uint8_t upload_slot;
static uint32_t upload_size;
static uint32_t upload_written;
static uint32_t upload_generation;
static uint32_t upload_crc;

static uint32_t slot_offset(uint8_t slot) { return (uint32_t)slot * PET_SLOT_SIZE; }
static uint32_t pack_offset(uint8_t slot) { return slot_offset(slot) + PET_SLOT_HEADER_AREA; }

uint32_t pet_crc32_update(uint32_t crc, const uint8_t *data, size_t size) {
    while (size-- > 0u) {
        crc ^= *data++;
        for (uint8_t bit = 0; bit < 8u; bit++) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

uint32_t pet_crc32(const uint8_t *data, size_t size) {
    return pet_crc32_update(0xffffffffu, data, size) ^ 0xffffffffu;
}

static int flash_crc32(uint32_t offset, uint32_t size, uint32_t *result) {
    uint8_t buffer[PET_IO_CHUNK_SIZE];
    uint32_t crc = 0xffffffffu;

    while (size > 0u) {
        size_t chunk = MIN((uint32_t)sizeof(buffer), size);
        int rc = flash_read(pet_flash, offset, buffer, chunk);
        if (rc < 0) {
            return rc;
        }
        crc = pet_crc32_update(crc, buffer, chunk);
        offset += chunk;
        size -= chunk;
    }

    *result = crc ^ 0xffffffffu;
    return 0;
}

static bool bounds_ok(uint32_t offset, uint32_t size, uint32_t total) {
    return offset <= total && size <= total - offset;
}

static int validate_pack(uint8_t slot, uint32_t advertised_size, struct pet_pack_header *header) {
    const uint32_t base = pack_offset(slot);
    int rc = flash_read(pet_flash, base, header, sizeof(*header));
    if (rc < 0) {
        return rc;
    }

    if (header->magic != PET_PACK_MAGIC || header->version != PET_PACK_VERSION ||
        header->header_size != sizeof(*header) || header->frame_width != PET_FRAME_WIDTH ||
        header->frame_height != PET_FRAME_HEIGHT || header->frame_count == 0u ||
        header->frame_count > PET_MAX_FRAMES || header->animation_count == 0u ||
        header->animation_count > PET_MAX_ANIMATIONS || header->total_size != advertised_size ||
        header->total_size > PET_SLOT_SIZE - PET_SLOT_HEADER_AREA) {
        return -EINVAL;
    }

    const uint32_t animation_bytes =
        (uint32_t)header->animation_count * sizeof(struct pet_animation_desc);
    const uint32_t frame_bytes = (uint32_t)header->frame_count * sizeof(struct pet_frame_desc);
    if (header->animation_table_offset != sizeof(*header) ||
        !bounds_ok(header->animation_table_offset, animation_bytes, header->total_size) ||
        header->frame_table_offset != header->animation_table_offset + animation_bytes ||
        !bounds_ok(header->frame_table_offset, frame_bytes, header->total_size) ||
        header->data_offset != header->frame_table_offset + frame_bytes) {
        return -EINVAL;
    }

    for (uint16_t index = 0; index < header->animation_count; index++) {
        struct pet_animation_desc animation;
        rc = flash_read(pet_flash,
                        base + header->animation_table_offset +
                            (uint32_t)index * sizeof(animation),
                        &animation, sizeof(animation));
        if (rc < 0) {
            return rc;
        }
        if (animation.id != index || animation.frame_count == 0u ||
            animation.first_frame >= header->frame_count ||
            animation.frame_count > header->frame_count - animation.first_frame) {
            return -EINVAL;
        }
    }

    for (uint16_t index = 0; index < header->frame_count; index++) {
        struct pet_frame_desc frame;
        rc = flash_read(pet_flash,
                        base + header->frame_table_offset + (uint32_t)index * sizeof(frame),
                        &frame, sizeof(frame));
        if (rc < 0) {
            return rc;
        }
        if (frame.duration_ms < 20u || frame.duration_ms > 10000u ||
            frame.data_offset < header->data_offset ||
            !bounds_ok(frame.data_offset, PET_FRAME_PACKED_BYTES, header->total_size)) {
            return -EINVAL;
        }
    }

    uint32_t content_crc;
    rc = flash_crc32(base + sizeof(*header), header->total_size - sizeof(*header), &content_crc);
    return rc < 0 ? rc : (content_crc == header->content_crc32 ? 0 : -EBADMSG);
}

static int validate_slot(uint8_t slot, struct pet_slot_header *slot_header,
                         struct pet_pack_header *pack_header) {
    int rc = flash_read(pet_flash, slot_offset(slot), slot_header, sizeof(*slot_header));
    if (rc < 0) {
        return rc;
    }
    if (slot_header->magic != PET_SLOT_MAGIC || slot_header->version != PET_SLOT_VERSION ||
        slot_header->header_size != sizeof(*slot_header) ||
        slot_header->flags != PET_SLOT_FLAG_COMMITTED || slot_header->pack_size == 0u ||
        slot_header->pack_size > PET_SLOT_SIZE - PET_SLOT_HEADER_AREA) {
        return -EINVAL;
    }

    uint32_t pack_crc;
    rc = flash_crc32(pack_offset(slot), slot_header->pack_size, &pack_crc);
    if (rc < 0 || pack_crc != slot_header->pack_crc32) {
        return rc < 0 ? rc : -EBADMSG;
    }
    return validate_pack(slot, slot_header->pack_size, pack_header);
}

static bool generation_newer(uint32_t candidate, uint32_t current) {
    return (int32_t)(candidate - current) > 0;
}

int pet_store_init(void) {
    k_mutex_lock(&store_mutex, K_FOREVER);
    if (initialized) {
        k_mutex_unlock(&store_mutex);
        return current_valid ? 0 : -ENOENT;
    }
    initialized = true;

    if (!device_is_ready(pet_flash)) {
        LOG_ERR("XIAO QSPI device is not ready; using embedded Merry fallback");
        k_mutex_unlock(&store_mutex);
        return -ENODEV;
    }

    for (uint8_t slot = 0; slot < PET_SLOT_COUNT; slot++) {
        struct pet_slot_header slot_header;
        struct pet_pack_header pack_header;
        if (validate_slot(slot, &slot_header, &pack_header) == 0 &&
            (!current_valid || generation_newer(slot_header.generation, current_generation))) {
            current_valid = true;
            current_slot = slot;
            current_generation = slot_header.generation;
            current_pack_size = slot_header.pack_size;
            current_pack = pack_header;
        }
    }

    if (current_valid) {
        LOG_INF("Merry pet pack: slot %u, generation %u, %u bytes", current_slot,
                current_generation, current_pack_size);
    } else {
        LOG_INF("No valid uploaded pet pack; using embedded Merry fallback");
    }
    k_mutex_unlock(&store_mutex);
    return current_valid ? 0 : -ENOENT;
}

static int pet_store_sys_init(void) {
    (void)pet_store_init();
    return 0;
}

SYS_INIT(pet_store_sys_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

bool pet_store_has_uploaded_pack(void) {
    k_mutex_lock(&store_mutex, K_FOREVER);
    bool result = current_valid;
    k_mutex_unlock(&store_mutex);
    return result;
}

int pet_store_get_animation(uint8_t animation_id, struct pet_animation_desc *animation) {
    if (animation == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&store_mutex, K_FOREVER);
    if (!current_valid) {
        if (animation_id != MERRY_ANIM_IDLE) {
            k_mutex_unlock(&store_mutex);
            return -ENOENT;
        }
        *animation = (struct pet_animation_desc){
            .id = MERRY_ANIM_IDLE, .flags = 1u, .first_frame = 0u, .frame_count = 1u};
        k_mutex_unlock(&store_mutex);
        return 0;
    }
    if (animation_id >= current_pack.animation_count) {
        k_mutex_unlock(&store_mutex);
        return -ENOENT;
    }

    int rc = flash_read(pet_flash,
                        pack_offset(current_slot) + current_pack.animation_table_offset +
                            (uint32_t)animation_id * sizeof(*animation),
                        animation, sizeof(*animation));
    k_mutex_unlock(&store_mutex);
    return rc;
}

int pet_store_read_frame(uint8_t animation_id, uint16_t animation_frame,
                         struct pet_frame *frame, uint8_t *packed_pixels,
                         size_t packed_pixels_size) {
    if (frame == NULL || packed_pixels == NULL || packed_pixels_size < PET_FRAME_PACKED_BYTES) {
        return -EINVAL;
    }

    k_mutex_lock(&store_mutex, K_FOREVER);
    if (!current_valid) {
        if (animation_id != MERRY_ANIM_IDLE || animation_frame != 0u) {
            k_mutex_unlock(&store_mutex);
            return -ENOENT;
        }
        frame->duration_ms = 1000u;
        memcpy(frame->palette, merry_fallback_palette, sizeof(frame->palette));
        memcpy(packed_pixels, merry_fallback_pixels, PET_FRAME_PACKED_BYTES);
        k_mutex_unlock(&store_mutex);
        return 0;
    }

    if (animation_id >= current_pack.animation_count) {
        k_mutex_unlock(&store_mutex);
        return -ENOENT;
    }

    struct pet_animation_desc animation;
    int rc = flash_read(pet_flash,
                        pack_offset(current_slot) + current_pack.animation_table_offset +
                            (uint32_t)animation_id * sizeof(animation),
                        &animation, sizeof(animation));
    if (rc < 0 || animation_frame >= animation.frame_count) {
        k_mutex_unlock(&store_mutex);
        return rc < 0 ? rc : -ERANGE;
    }

    struct pet_frame_desc stored_frame;
    const uint16_t frame_index = animation.first_frame + animation_frame;
    rc = flash_read(pet_flash,
                    pack_offset(current_slot) + current_pack.frame_table_offset +
                        (uint32_t)frame_index * sizeof(stored_frame),
                    &stored_frame, sizeof(stored_frame));
    if (rc == 0) {
        rc = flash_read(pet_flash, pack_offset(current_slot) + stored_frame.data_offset,
                        packed_pixels, PET_FRAME_PACKED_BYTES);
    }
    if (rc == 0) {
        frame->duration_ms = stored_frame.duration_ms;
        memcpy(frame->palette, stored_frame.palette, sizeof(frame->palette));
    }
    k_mutex_unlock(&store_mutex);
    return rc;
}

size_t pet_store_max_upload_size(void) { return PET_SLOT_SIZE - PET_SLOT_HEADER_AREA; }

int pet_store_upload_begin(uint32_t pack_size) {
    if (pack_size < sizeof(struct pet_pack_header) || pack_size > pet_store_max_upload_size() ||
        (pack_size & 3u) != 0u) {
        return -EFBIG;
    }
    if (!initialized) {
        (void)pet_store_init();
    }

    k_mutex_lock(&store_mutex, K_FOREVER);
    if (!device_is_ready(pet_flash) || upload_active) {
        k_mutex_unlock(&store_mutex);
        return upload_active ? -EBUSY : -ENODEV;
    }

    upload_slot = current_valid ? (uint8_t)(1u - current_slot) : 0u;
    upload_size = pack_size;
    upload_written = 0u;
    upload_generation = current_valid ? current_generation + 1u : 1u;
    upload_crc = 0xffffffffu;

    int rc = flash_erase(pet_flash, slot_offset(upload_slot), PET_SLOT_SIZE);
    if (rc == 0) {
        upload_active = true;
    }
    k_mutex_unlock(&store_mutex);
    return rc;
}

int pet_store_upload_write(uint32_t offset, const uint8_t *data, size_t size) {
    if (data == NULL || size == 0u) {
        return -EINVAL;
    }

    k_mutex_lock(&store_mutex, K_FOREVER);
    if (!upload_active || offset != upload_written || size > upload_size - upload_written) {
        k_mutex_unlock(&store_mutex);
        return !upload_active ? -EACCES : -EINVAL;
    }
    int rc = flash_write(pet_flash, pack_offset(upload_slot) + offset, data, size);
    if (rc == 0) {
        upload_crc = pet_crc32_update(upload_crc, data, size);
        upload_written += size;
    }
    k_mutex_unlock(&store_mutex);
    return rc;
}

int pet_store_upload_finish(uint32_t expected_crc32) {
    k_mutex_lock(&store_mutex, K_FOREVER);
    if (!upload_active || upload_written != upload_size ||
        (upload_crc ^ 0xffffffffu) != expected_crc32) {
        upload_active = false;
        k_mutex_unlock(&store_mutex);
        return -EBADMSG;
    }

    struct pet_pack_header candidate_pack;
    int rc = validate_pack(upload_slot, upload_size, &candidate_pack);
    if (rc == 0) {
        struct pet_slot_header slot_header = {
            .magic = PET_SLOT_MAGIC,
            .version = PET_SLOT_VERSION,
            .header_size = sizeof(struct pet_slot_header),
            .generation = upload_generation,
            .pack_size = upload_size,
            .pack_crc32 = expected_crc32,
            .flags = PET_SLOT_FLAG_COMMITTED,
        };
        rc = flash_write(pet_flash, slot_offset(upload_slot), &slot_header, sizeof(slot_header));
    }

    if (rc == 0) {
        current_valid = true;
        current_slot = upload_slot;
        current_generation = upload_generation;
        current_pack_size = upload_size;
        current_pack = candidate_pack;
    }
    upload_active = false;
    k_mutex_unlock(&store_mutex);
    return rc;
}

void pet_store_upload_abort(void) {
    k_mutex_lock(&store_mutex, K_FOREVER);
    upload_active = false;
    k_mutex_unlock(&store_mutex);
}
