/* SPDX-License-Identifier: MIT */

#include "merry_config.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include "pet_pack.h"

#define MERRY_SETTINGS_KEY "merry/config"
#define MERRY_SETTINGS_MAGIC 0x31474643u /* CFG1 */

struct merry_persisted_config {
    uint32_t magic;
    struct merry_config config;
} __packed;

BUILD_ASSERT(sizeof(struct merry_config) == 16u, "Merry config wire layout changed");
BUILD_ASSERT(sizeof(struct merry_persisted_config) == 20u,
             "Merry persisted config layout changed");

static const struct merry_config default_config = {
    .version = MERRY_CONFIG_VERSION,
    .display_mode = MERRY_DISPLAY_AUTO,
    .animation_id = MERRY_ANIM_IDLE,
    .brightness = 100u,
    .idle_timeout_ms = MERRY_CONFIG_DEFAULT_TIMEOUT_MS,
};

static struct merry_config current_config = {
    .version = MERRY_CONFIG_VERSION,
    .display_mode = MERRY_DISPLAY_AUTO,
    .animation_id = MERRY_ANIM_IDLE,
    .brightness = 100u,
    .idle_timeout_ms = MERRY_CONFIG_DEFAULT_TIMEOUT_MS,
};

K_MUTEX_DEFINE(config_mutex);

bool merry_config_is_valid(const struct merry_config *config) {
    return config != NULL && config->version == MERRY_CONFIG_VERSION &&
           config->display_mode <= MERRY_DISPLAY_OFF &&
           config->animation_id <= MERRY_ANIM_BLOCKED && config->brightness <= 100u &&
           config->idle_timeout_ms >= MERRY_CONFIG_MIN_TIMEOUT_MS &&
           config->idle_timeout_ms <= MERRY_CONFIG_MAX_TIMEOUT_MS &&
           config->pet_x >= MERRY_CONFIG_MIN_X && config->pet_x <= MERRY_CONFIG_MAX_X &&
           config->pet_y >= MERRY_CONFIG_MIN_Y && config->pet_y <= MERRY_CONFIG_MAX_Y &&
           config->reserved == 0u;
}

int merry_config_get(struct merry_config *config) {
    if (config == NULL) {
        return -EINVAL;
    }
    k_mutex_lock(&config_mutex, K_FOREVER);
    *config = current_config;
    k_mutex_unlock(&config_mutex);
    return 0;
}

static int save_config(const struct merry_config *config) {
    const struct merry_persisted_config persisted = {
        .magic = MERRY_SETTINGS_MAGIC,
        .config = *config,
    };
    return settings_save_one(MERRY_SETTINGS_KEY, &persisted, sizeof(persisted));
}

int merry_config_set(const struct merry_config *config) {
    if (!merry_config_is_valid(config)) {
        return -EINVAL;
    }
    int rc = save_config(config);
    if (rc < 0) {
        return rc;
    }
    k_mutex_lock(&config_mutex, K_FOREVER);
    current_config = *config;
    k_mutex_unlock(&config_mutex);
    merry_screen_config_changed();
    return 0;
}

int merry_config_reset(void) { return merry_config_set(&default_config); }

static int merry_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                              void *cb_arg) {
    const char *next;
    if (!settings_name_steq(name, "config", &next) || next != NULL) {
        return -ENOENT;
    }
    if (len != sizeof(struct merry_persisted_config)) {
        return -EINVAL;
    }

    struct merry_persisted_config persisted;
    int rc = read_cb(cb_arg, &persisted, sizeof(persisted));
    if (rc < 0) {
        return rc;
    }
    if (persisted.magic != MERRY_SETTINGS_MAGIC || !merry_config_is_valid(&persisted.config)) {
        return -EINVAL;
    }
    current_config = persisted.config;
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(merry_config, "merry", NULL, merry_settings_set, NULL, NULL);
