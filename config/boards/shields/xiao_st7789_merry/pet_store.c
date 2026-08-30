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
BUILD_ASSERT(sizeof(struct pet_frame_desc) == 136u, "frame descriptor layout changed");
BUILD_ASSERT(PET_FRAME_PACKED_BYTES == MERRY_FALLBACK_DATA_SIZE,
             "fallback and decoder dimensions disagree");
BUILD_ASSERT((PET_FRAME_PACKED_BYTES % 4u) == 0u,
             "Nordic QSPI frame reads must be word-sized");

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
static bool current_quarantined;
static int32_t current_last_error;

static bool upload_active;
static uint8_t upload_slot;
static uint32_t upload_size;
static uint32_t upload_written;
static uint32_t upload_generation;
static uint32_t upload_crc;

static uint32_t slot_offset(uint8_t slot) { return (uint32_t)slot * PET_SLOT_SIZE; }
static uint32_t pack_offset(uint8_t slot) { return slot_offset(slot) + PET_SLOT_HEADER_AREA; }

static int flash_read_chunked(uint32_t offset, void *destination, size_t size) {
    uint8_t *bytes = destination;

    while (size > 0u) {
        const size_t chunk = MIN(size, (size_t)PET_IO_CHUNK_SIZE);
        int rc = flash_read(pet_flash, offset, bytes, chunk);
        if (rc < 0) {
            return rc;
        }
        offset += chunk;
        bytes += chunk;
        size -= chunk;
    }
    return 0;
}

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

static int validate_pack(uint8_t slot, uint32_t advertised_size, struct pet_pack_header *header,
                         bool verify_content_crc) {
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

    if (verify_content_crc) {
        uint32_t content_crc;
        rc = flash_crc32(base + sizeof(*header), header->total_size - sizeof(*header),
                         &content_crc);
        return rc < 0 ? rc : (content_crc == header->content_crc32 ? 0 : -EBADMSG);
    }
    return 0;
}

static int validate_slot(uint8_t slot, struct pet_slot_header *slot_header,
                         struct pet_pack_header *pack_header, bool verify_crc) {
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

    if (verify_crc) {
        uint32_t pack_crc;
        rc = flash_crc32(pack_offset(slot), slot_header->pack_size, &pack_crc);
        if (rc < 0 || pack_crc != slot_header->pack_crc32) {
            return rc < 0 ? rc : -EBADMSG;
        }
    }
    return validate_pack(slot, slot_header->pack_size, pack_header, verify_crc);
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
        /* A committed slot was fully CRC-checked before its header was written.
         * At boot, check its structure only. Re-reading and software-CRCing up
         * to two complete 1 MiB slots here can stall USB and display startup.
         */
        if (validate_slot(slot, &slot_header, &pack_header, false) == 0 &&
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

void pet_store_get_status(struct pet_store_status *status) {
    if (status == NULL) {
        return;
    }
    k_mutex_lock(&store_mutex, K_FOREVER);
    *status = (struct pet_store_status){
        .pack_version = PET_PACK_VERSION,
        .uploaded_pack_active = current_valid ? 1u : 0u,
        .active_slot = current_valid ? current_slot : 0xffu,
        .quarantined = current_quarantined ? 1u : 0u,
        .generation = current_generation,
        .pack_size = current_pack_size,
        .last_error = current_last_error,
    };
    k_mutex_unlock(&store_mutex);
}

static void quarantine_current_locked(int error) {
    if (current_valid) {
        LOG_ERR("Merry pack read failed (%d); using embedded fallback", error);
    }
    current_valid = false;
    current_quarantined = true;
    current_last_error = error;
}

int pet_store_clear_uploads(void) {
    if (!device_is_ready(pet_flash)) {
        return -ENODEV;
    }

    k_mutex_lock(&store_mutex, K_FOREVER);
    if (upload_active) {
        k_mutex_unlock(&store_mutex);
        return -EBUSY;
    }
    /* Stop using external data before erasing. Even if one erase fails, the
     * running firmware must remain on the embedded fallback.
     */
    current_valid = false;
    current_slot = 0u;
    current_generation = 0u;
    current_pack_size = 0u;
    memset(&current_pack, 0, sizeof(current_pack));
    current_quarantined = false;
    current_last_error = 0;

    int first_rc = flash_erase(pet_flash, slot_offset(0u), PET_SLOT_SIZE);
    int second_rc = flash_erase(pet_flash, slot_offset(1u), PET_SLOT_SIZE);
    int rc = first_rc < 0 ? first_rc : second_rc;
    if (rc < 0) {
        current_quarantined = true;
        current_last_error = rc;
    }
    k_mutex_unlock(&store_mutex);
    return rc;
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
    if (rc == 0 &&
        (animation->id != animation_id || animation->frame_count == 0u ||
         animation->first_frame >= current_pack.frame_count ||
         animation->frame_count > current_pack.frame_count - animation->first_frame)) {
        rc = -EBADMSG;
    }
    if (rc < 0) {
        quarantine_current_locked(rc);
    }
    k_mutex_unlock(&store_mutex);
    return rc;
}

static void copy_embedded_fallback(struct pet_frame *frame, uint8_t *packed_pixels) {
    frame->duration_ms = 1000u;
    memcpy(frame->palette, merry_fallback_palette, sizeof(frame->palette));
    memcpy(packed_pixels, merry_fallback_pixels, PET_FRAME_PACKED_BYTES);
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
        copy_embedded_fallback(frame, packed_pixels);
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
    if (rc == 0 &&
        (animation.id != animation_id || animation.frame_count == 0u ||
         animation.first_frame >= current_pack.frame_count ||
         animation.frame_count > current_pack.frame_count - animation.first_frame)) {
        rc = -EBADMSG;
    }
    if (rc < 0) {
        quarantine_current_locked(rc);
        copy_embedded_fallback(frame, packed_pixels);
        k_mutex_unlock(&store_mutex);
        return 0;
    }
    if (animation_frame >= animation.frame_count) {
        k_mutex_unlock(&store_mutex);
        return -ERANGE;
    }

    struct pet_frame_desc stored_frame;
    const uint16_t frame_index = animation.first_frame + animation_frame;
    rc = flash_read(pet_flash,
                    pack_offset(current_slot) + current_pack.frame_table_offset +
                        (uint32_t)frame_index * sizeof(stored_frame),
                    &stored_frame, sizeof(stored_frame));
    if (rc == 0 &&
        (stored_frame.duration_ms < 20u || stored_frame.duration_ms > 10000u ||
         stored_frame.data_offset < current_pack.data_offset ||
         !bounds_ok(stored_frame.data_offset, PET_FRAME_PACKED_BYTES,
                    current_pack.total_size))) {
        rc = -EBADMSG;
    }
    if (rc == 0) {
        rc = flash_read_chunked(pack_offset(current_slot) + stored_frame.data_offset,
                                packed_pixels, PET_FRAME_PACKED_BYTES);
    }
    if (rc == 0) {
        frame->duration_ms = stored_frame.duration_ms;
        memcpy(frame->palette, stored_frame.palette, sizeof(frame->palette));
    }
    if (rc < 0) {
        quarantine_current_locked(rc);
        copy_embedded_fallback(frame, packed_pixels);
        rc = 0;
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

    /* A quarantined slot still participates in generation ordering. Replace
     * the other slot with a newer generation instead of accidentally writing
     * generation 1 and allowing the quarantined generation to win on reboot.
     */
    upload_slot = current_generation > 0u ? (uint8_t)(1u - current_slot) : 0u;
    upload_size = pack_size;
    upload_written = 0u;
    upload_generation = current_generation + 1u;
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
    int rc = validate_pack(upload_slot, upload_size, &candidate_pack, true);
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
        current_quarantined = false;
        current_last_error = 0;
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
