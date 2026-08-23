/* SPDX-License-Identifier: MIT */

#define DT_DRV_COMPAT ayaan_as5600_scroll

#include <errno.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(as5600_scroll, CONFIG_INPUT_LOG_LEVEL);

#define AS5600_REG_STATUS 0x0B
#define AS5600_REG_RAW_ANGLE 0x0C
#define AS5600_REG_AGC 0x1A
#define AS5600_STATUS_MAGNET_DETECTED BIT(5)
#define AS5600_AGC_TRUST_MIN 30
#define AS5600_AGC_TRUST_MAX 90
#define AS5600_COUNTS_PER_REVOLUTION 4096
#define AS5600_HALF_REVOLUTION (AS5600_COUNTS_PER_REVOLUTION / 2)

struct as5600_scroll_config {
    struct i2c_dt_spec bus;
    uint16_t poll_interval_ms;
    uint16_t startup_delay_ms;
    uint16_t counts_per_scroll;
    uint16_t max_delta_counts;
    uint16_t deadband_counts;
    uint32_t acceleration_threshold;
    uint8_t acceleration_multiplier;
    uint32_t fast_threshold;
    uint8_t fast_multiplier;
    bool invert_scroll;
};

struct as5600_scroll_data {
    const struct device *dev;
    struct k_work_delayable work;
    int32_t accumulator;
    uint16_t previous_angle;
    bool initialized;
    bool read_error_logged;
    bool diagnostics_logged;
#if IS_ENABLED(CONFIG_INPUT_AS5600_SCROLL_DIAGNOSTICS)
    uint32_t diag_polls;
    uint16_t diag_min;
    uint16_t diag_max;
    uint32_t diag_rejected;
#endif
};

static void as5600_log_diagnostics(const struct device *dev) {
    const struct as5600_scroll_config *cfg = dev->config;
    uint8_t status;
    uint8_t agc;
    int err;

    err = i2c_reg_read_byte_dt(&cfg->bus, AS5600_REG_STATUS, &status);
    if (err < 0) {
        LOG_ERR("AS5600 did not ACK at 0x36; check 3V3/GND, SDA D4, and SCL D3");
        return;
    }

    if (!(status & AS5600_STATUS_MAGNET_DETECTED)) {
        LOG_WRN("AS5600 ACKed at 0x36, but no magnet is detected");
    }

    err = i2c_reg_read_byte_dt(&cfg->bus, AS5600_REG_AGC, &agc);
    if (err < 0) {
        LOG_WRN("AS5600 AGC read failed: %d", err);
    } else if (agc < AS5600_AGC_TRUST_MIN || agc > AS5600_AGC_TRUST_MAX) {
        LOG_WRN("AS5600 AGC=%u is outside the 3.3 V target range %u-%u", (unsigned int)agc,
                (unsigned int)AS5600_AGC_TRUST_MIN, (unsigned int)AS5600_AGC_TRUST_MAX);
    } else {
        LOG_INF("AS5600 AGC=%u (3.3 V target %u-%u)", (unsigned int)agc,
                (unsigned int)AS5600_AGC_TRUST_MIN, (unsigned int)AS5600_AGC_TRUST_MAX);
    }
}

static int as5600_read_angle(const struct as5600_scroll_config *cfg, uint16_t *angle) {
    uint8_t raw[2];
    int err = i2c_burst_read_dt(&cfg->bus, AS5600_REG_RAW_ANGLE, raw, sizeof(raw));

    if (err < 0) {
        return err;
    }

    *angle = (((uint16_t)raw[0] << 8) | raw[1]) & 0x0FFF;
    return 0;
}

static int16_t as5600_wrapped_delta(uint16_t current, uint16_t previous) {
    int16_t delta = (int16_t)current - (int16_t)previous;

    if (delta > AS5600_HALF_REVOLUTION) {
        delta -= AS5600_COUNTS_PER_REVOLUTION;
    } else if (delta < -AS5600_HALF_REVOLUTION) {
        delta += AS5600_COUNTS_PER_REVOLUTION;
    }

    return delta;
}

static uint8_t as5600_acceleration(const struct as5600_scroll_config *cfg, int16_t delta) {
    uint32_t magnitude = delta < 0 ? (uint32_t)(-(int32_t)delta) : (uint32_t)delta;
    uint32_t speed = (magnitude * 1000U) / cfg->poll_interval_ms;

    if (speed >= cfg->fast_threshold) {
        return cfg->fast_multiplier;
    }
    if (speed >= cfg->acceleration_threshold) {
        return cfg->acceleration_multiplier;
    }
    return 1;
}

static void as5600_scroll_work_cb(struct k_work *work) {
    struct k_work_delayable *dwork = CONTAINER_OF(work, struct k_work_delayable, work);
    struct as5600_scroll_data *data = CONTAINER_OF(dwork, struct as5600_scroll_data, work);
    const struct device *dev = data->dev;
    const struct as5600_scroll_config *cfg = dev->config;
    uint16_t angle;
    int16_t delta;
    uint32_t magnitude;
    int32_t wheel;
    int err = as5600_read_angle(cfg, &angle);

    if (err < 0) {
        if (!data->read_error_logged) {
            LOG_ERR("AS5600 did not ACK at 0x36; check 3V3/GND, SDA D4, and SCL D3 (%d)", err);
            data->read_error_logged = true;
        }
        goto reschedule;
    }
    data->read_error_logged = false;

    if (!data->diagnostics_logged) {
        as5600_log_diagnostics(dev);
        data->diagnostics_logged = true;
    }

    if (!data->initialized) {
        data->previous_angle = angle;
        data->initialized = true;
#if IS_ENABLED(CONFIG_INPUT_AS5600_SCROLL_DIAGNOSTICS)
        data->diag_polls = 0;
        data->diag_rejected = 0;
        data->diag_min = angle;
        data->diag_max = angle;
#endif
        goto reschedule;
    }

    delta = as5600_wrapped_delta(angle, data->previous_angle);
    magnitude = (delta < 0) ? (uint32_t)(-(int32_t)delta) : (uint32_t)delta;

    if (cfg->max_delta_counts != 0 && magnitude > cfg->max_delta_counts) {
        /* A hand-turned knob cannot move this far between two polls: at the
         * default 4 ms / 512 counts that is over 30 rev/s. Such a jump is a
         * sensor artefact, in practice a weak magnet letting mains hum swing
         * the measured field. Resync the reference and emit nothing, so a
         * single bad sample cannot become hundreds of wheel clicks by way of
         * the acceleration multiplier. */
        data->previous_angle = angle;
#if IS_ENABLED(CONFIG_INPUT_AS5600_SCROLL_DIAGNOSTICS)
        data->diag_rejected++;
#endif
    } else if (magnitude >= cfg->deadband_counts) {
        data->previous_angle = angle;

        if (cfg->invert_scroll) {
            delta = -delta;
        }

        data->accumulator += (int32_t)delta * as5600_acceleration(cfg, delta);

        wheel = data->accumulator / cfg->counts_per_scroll;
        if (wheel != 0) {
            wheel = CLAMP(wheel, INT16_MIN, INT16_MAX);
            data->accumulator -= wheel * cfg->counts_per_scroll;
            input_report_rel(dev, INPUT_REL_WHEEL, (int16_t)wheel, true, K_NO_WAIT);
#if IS_ENABLED(CONFIG_INPUT_AS5600_SCROLL_DIAGNOSTICS)
            LOG_INF("dial: wheel=%d angle=%u delta=%d", (int)wheel, (unsigned int)angle,
                    (int)delta);
#endif
        }
    }
    /* Otherwise the movement is inside the deadband, and previous_angle is
     * deliberately left alone: slow genuine rotation keeps accumulating
     * against the held reference instead of being erased sample by sample. */

#if IS_ENABLED(CONFIG_INPUT_AS5600_SCROLL_DIAGNOSTICS)
    /* Heartbeat that separates "AS5600 is static" from "moving but emitting
     * no wheel events", without logging at the poll rate. span is the raw
     * min/max window, so a pass through 0 reads large; that is expected. */
    if (angle < data->diag_min) {
        data->diag_min = angle;
    }
    if (angle > data->diag_max) {
        data->diag_max = angle;
    }
    if (++data->diag_polls * cfg->poll_interval_ms >= 1000U) {
        LOG_INF("dial: angle=%u span=%u accum=%d rejected=%u", (unsigned int)angle,
                (unsigned int)(data->diag_max - data->diag_min), (int)data->accumulator,
                (unsigned int)data->diag_rejected);
        data->diag_polls = 0;
        data->diag_rejected = 0;
        data->diag_min = angle;
        data->diag_max = angle;
    }
#endif

reschedule:
    k_work_schedule(&data->work, K_MSEC(cfg->poll_interval_ms));
}

static int as5600_scroll_init(const struct device *dev) {
    const struct as5600_scroll_config *cfg = dev->config;
    struct as5600_scroll_data *data = dev->data;

    if (!i2c_is_ready_dt(&cfg->bus)) {
        LOG_ERR("I2C bus is not ready");
        return -ENODEV;
    }
    if (cfg->poll_interval_ms == 0 || cfg->counts_per_scroll == 0 ||
        cfg->acceleration_multiplier == 0 || cfg->fast_multiplier == 0) {
        LOG_ERR("poll interval, scroll divisor, and multipliers must be nonzero");
        return -EINVAL;
    }

    data->dev = dev;
    k_work_init_delayable(&data->work, as5600_scroll_work_cb);
    k_work_schedule(&data->work, K_MSEC(cfg->startup_delay_ms));
    return 0;
}

#define AS5600_SCROLL_INST(n)                                                                     \
    static struct as5600_scroll_data as5600_scroll_data_##n;                                      \
    static const struct as5600_scroll_config as5600_scroll_config_##n = {                         \
        .bus = I2C_DT_SPEC_INST_GET(n),                                                           \
        .poll_interval_ms = DT_INST_PROP(n, poll_interval_ms),                                    \
        .startup_delay_ms = DT_INST_PROP(n, startup_delay_ms),                                    \
        .counts_per_scroll = DT_INST_PROP(n, counts_per_scroll),                                  \
        .max_delta_counts = DT_INST_PROP(n, max_delta_counts),                                    \
        .deadband_counts = DT_INST_PROP(n, deadband_counts),                                      \
        .acceleration_threshold = DT_INST_PROP(n, acceleration_threshold),                        \
        .acceleration_multiplier = DT_INST_PROP(n, acceleration_multiplier),                      \
        .fast_threshold = DT_INST_PROP(n, fast_threshold),                                        \
        .fast_multiplier = DT_INST_PROP(n, fast_multiplier),                                      \
        .invert_scroll = DT_INST_PROP(n, invert_scroll),                                          \
    };                                                                                            \
    DEVICE_DT_INST_DEFINE(n, as5600_scroll_init, NULL, &as5600_scroll_data_##n,                   \
                          &as5600_scroll_config_##n, POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(AS5600_SCROLL_INST)
