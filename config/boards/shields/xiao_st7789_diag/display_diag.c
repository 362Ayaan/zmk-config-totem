/* SPDX-License-Identifier: MIT */

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define DIAG_SPI_NODE DT_ALIAS(merry_diag_spi)
#define DIAG_GPIO_NODE DT_NODELABEL(gpio0)

/* Verified XIAO nRF52840 pin mapping: D0=P0.02, D1=P0.03, D2=P0.28. */
#define DC_PIN 2u
#define RESET_PIN 3u
#define BACKLIGHT_PIN 28u

#define LCD_WIDTH 240u
#define LCD_HEIGHT 240u
#define LCD_SPI_HZ 1000000u

#define CMD_SWRESET 0x01u
#define CMD_SLPOUT 0x11u
#define CMD_NORON 0x13u
#define CMD_INVON 0x21u
#define CMD_DISPON 0x29u
#define CMD_CASET 0x2au
#define CMD_RASET 0x2bu
#define CMD_RAMWR 0x2cu
#define CMD_MADCTL 0x36u
#define CMD_COLMOD 0x3au

BUILD_ASSERT(DT_NODE_HAS_STATUS(DIAG_SPI_NODE, okay), "Diagnostic SPI bus must be enabled");
BUILD_ASSERT(DT_NODE_HAS_STATUS(DIAG_GPIO_NODE, okay), "Diagnostic GPIO port must be enabled");

static const struct device *const diag_spi = DEVICE_DT_GET(DIAG_SPI_NODE);
static const struct device *const diag_gpio = DEVICE_DT_GET(DIAG_GPIO_NODE);

static const struct spi_config spi_config = {
    .frequency = LCD_SPI_HZ,
    .operation = SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8),
    .slave = 0,
};

static uint8_t line_buffer[LCD_WIDTH * 2u];

static void signal_runtime_error(void) {
    /* A fast repeating flash means Zephyr rejected an SPI/GPIO operation. */
    while (true) {
        gpio_pin_set(diag_gpio, BACKLIGHT_PIN, 0);
        k_sleep(K_MSEC(100));
        gpio_pin_set(diag_gpio, BACKLIGHT_PIN, 1);
        k_sleep(K_MSEC(100));
    }
}

static int spi_send(const uint8_t *data, size_t length) {
    const struct spi_buf buffer = {.buf = (void *)data, .len = length};
    const struct spi_buf_set buffers = {.buffers = &buffer, .count = 1u};
    return spi_write(diag_spi, &spi_config, &buffers);
}

static int write_command(uint8_t command, const uint8_t *data, size_t length) {
    int rc = gpio_pin_set(diag_gpio, DC_PIN, 0);
    if (rc < 0) {
        return rc;
    }
    rc = spi_send(&command, sizeof(command));
    if (rc < 0 || length == 0u) {
        return rc;
    }
    rc = gpio_pin_set(diag_gpio, DC_PIN, 1);
    return rc < 0 ? rc : spi_send(data, length);
}

static int panel_init(void) {
    /* Start with an intentionally generous power-up and reset sequence. */
    k_sleep(K_MSEC(200));
    gpio_pin_set(diag_gpio, RESET_PIN, 1);
    k_sleep(K_MSEC(20));
    gpio_pin_set(diag_gpio, RESET_PIN, 0);
    k_sleep(K_MSEC(20));
    gpio_pin_set(diag_gpio, RESET_PIN, 1);
    k_sleep(K_MSEC(150));

    int rc = write_command(CMD_SWRESET, NULL, 0u);
    if (rc < 0) {
        return rc;
    }
    k_sleep(K_MSEC(150));

    rc = write_command(CMD_SLPOUT, NULL, 0u);
    if (rc < 0) {
        return rc;
    }
    k_sleep(K_MSEC(150));

    const uint8_t colmod = 0x55u; /* Standard 16-bit RGB565 interface format. */
    const uint8_t madctl = 0x00u;
    rc = write_command(CMD_COLMOD, &colmod, sizeof(colmod));
    if (rc < 0) {
        return rc;
    }
    rc = write_command(CMD_MADCTL, &madctl, sizeof(madctl));
    if (rc < 0) {
        return rc;
    }
    rc = write_command(CMD_INVON, NULL, 0u);
    if (rc < 0) {
        return rc;
    }
    rc = write_command(CMD_NORON, NULL, 0u);
    if (rc < 0) {
        return rc;
    }
    k_sleep(K_MSEC(10));
    rc = write_command(CMD_DISPON, NULL, 0u);
    k_sleep(K_MSEC(100));
    return rc;
}

static int set_full_window(void) {
    static const uint8_t bounds[] = {0x00u, 0x00u, 0x00u, 0xefu};
    int rc = write_command(CMD_CASET, bounds, sizeof(bounds));
    if (rc < 0) {
        return rc;
    }
    return write_command(CMD_RASET, bounds, sizeof(bounds));
}

static int fill_screen(uint16_t rgb565) {
    for (size_t pixel = 0; pixel < LCD_WIDTH; pixel++) {
        line_buffer[pixel * 2u] = (uint8_t)(rgb565 >> 8);
        line_buffer[pixel * 2u + 1u] = (uint8_t)rgb565;
    }

    int rc = set_full_window();
    if (rc < 0) {
        return rc;
    }
    rc = write_command(CMD_RAMWR, NULL, 0u);
    if (rc < 0) {
        return rc;
    }
    rc = gpio_pin_set(diag_gpio, DC_PIN, 1);
    if (rc < 0) {
        return rc;
    }
    for (size_t row = 0; row < LCD_HEIGHT; row++) {
        rc = spi_send(line_buffer, sizeof(line_buffer));
        if (rc < 0) {
            return rc;
        }
    }
    return 0;
}

static void diagnostic_thread(void *unused1, void *unused2, void *unused3) {
    ARG_UNUSED(unused1);
    ARG_UNUSED(unused2);
    ARG_UNUSED(unused3);

    if (!device_is_ready(diag_spi) || !device_is_ready(diag_gpio)) {
        return;
    }

    if (gpio_pin_configure(diag_gpio, DC_PIN, GPIO_OUTPUT_INACTIVE) < 0 ||
        gpio_pin_configure(diag_gpio, RESET_PIN, GPIO_OUTPUT_ACTIVE) < 0 ||
        gpio_pin_configure(diag_gpio, BACKLIGHT_PIN, GPIO_OUTPUT_INACTIVE) < 0) {
        return;
    }

    /* Three visible backlight flashes identify this diagnostic firmware. */
    for (uint8_t flash = 0; flash < 3u; flash++) {
        gpio_pin_set(diag_gpio, BACKLIGHT_PIN, 1);
        k_sleep(K_MSEC(250));
        gpio_pin_set(diag_gpio, BACKLIGHT_PIN, 0);
        k_sleep(K_MSEC(250));
    }
    gpio_pin_set(diag_gpio, BACKLIGHT_PIN, 1);

    if (panel_init() < 0) {
        signal_runtime_error();
    }

    static const uint16_t colors[] = {
        0xf800u, /* red */
        0x07e0u, /* green */
        0x001fu, /* blue */
        0xffffu, /* white */
        0x0000u, /* black */
    };

    while (true) {
        for (size_t index = 0; index < ARRAY_SIZE(colors); index++) {
            if (fill_screen(colors[index]) < 0) {
                signal_runtime_error();
            }
            k_sleep(K_SECONDS(2));
        }
    }
}

K_THREAD_DEFINE(merry_display_diag_thread, 2048, diagnostic_thread, NULL, NULL, NULL, 12, 0, 0);
