/* SPDX-License-Identifier: MIT */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "pinmap.h"

#define MERRY_LCD_WIDTH 240
#define MERRY_LCD_HEIGHT 240
#define MERRY_LCD_PIXELS ((size_t)MERRY_LCD_WIDTH * MERRY_LCD_HEIGHT)
#define MERRY_LCD_FRAME_BYTES (MERRY_LCD_PIXELS * sizeof(uint16_t))
#define MERRY_LCD_SPI_HZ (40 * 1000 * 1000)

static const char *const TAG = "merry-display";
static SemaphoreHandle_t transfer_done;

static inline uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue) {
    return (uint16_t)(((uint16_t)(red & 0xf8u) << 8) |
                      ((uint16_t)(green & 0xfcu) << 3) | (blue >> 3));
}

static bool color_transfer_done(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t *event,
                                void *user_context) {
    (void)panel_io;
    (void)event;
    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_context, &task_woken);
    return task_woken == pdTRUE;
}

static void backlight_init(void) {
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = MERRY_TFT_PIN_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 1023,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

static esp_lcd_panel_handle_t display_init(void) {
    const spi_bus_config_t bus = {
        .sclk_io_num = MERRY_TFT_PIN_SCLK,
        .mosi_io_num = MERRY_TFT_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = MERRY_LCD_FRAME_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = MERRY_TFT_PIN_DC,
        .cs_gpio_num = -1,
        .pclk_hz = MERRY_LCD_SPI_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 3,
        .trans_queue_depth = 2,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                              &io_config, &io));

    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = color_transfer_done,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io, &callbacks,
                                                               transfer_done));

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = MERRY_TFT_PIN_RESET,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        /* Xtensa stores uint16_t RGB565 pixels least-significant byte first. */
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    return panel;
}

static void render_calibration(uint16_t *frame) {
    static const uint16_t bars[] = {
        0x0000, 0xffff, 0xf800, 0x07e0, 0x001f, 0xffe0, 0xf81f, 0x07ff,
    };
    for (int y = 0; y < MERRY_LCD_HEIGHT; ++y) {
        for (int x = 0; x < MERRY_LCD_WIDTH; ++x) {
            if (y < 120) {
                frame[y * MERRY_LCD_WIDTH + x] = bars[(x * 8) / MERRY_LCD_WIDTH];
            } else if (y < 160) {
                frame[y * MERRY_LCD_WIDTH + x] = rgb565((uint8_t)x, 0, 0);
            } else if (y < 200) {
                frame[y * MERRY_LCD_WIDTH + x] = rgb565(0, (uint8_t)x, 0);
            } else {
                frame[y * MERRY_LCD_WIDTH + x] = rgb565(0, 0, (uint8_t)x);
            }
        }
    }
}

static void render_animation(uint16_t *frame, uint32_t phase) {
    for (uint32_t y = 0; y < MERRY_LCD_HEIGHT; ++y) {
        for (uint32_t x = 0; x < MERRY_LCD_WIDTH; ++x) {
            const uint8_t red = (uint8_t)((x + phase * 3u) ^ (y >> 1));
            const uint8_t green = (uint8_t)((y + phase * 2u) ^ (x >> 1));
            const uint8_t blue = (uint8_t)((x + y + phase * 5u) ^ (x >> 2));
            frame[y * MERRY_LCD_WIDTH + x] = rgb565(red, green, blue);
        }
    }
}

static int64_t flush_frame(esp_lcd_panel_handle_t panel, uint16_t *frame) {
    const int64_t started = esp_timer_get_time();
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, MERRY_LCD_WIDTH,
                                               MERRY_LCD_HEIGHT, frame));
    ESP_ERROR_CHECK(xSemaphoreTake(transfer_done, pdMS_TO_TICKS(1000)) == pdTRUE
                        ? ESP_OK
                        : ESP_ERR_TIMEOUT);
    return esp_timer_get_time() - started;
}

void app_main(void) {
    ESP_LOGI(TAG, "Merry ESP32-S3 graphics bring-up");
    ESP_LOGI(TAG, "Target: regular XIAO ESP32-S3, ST7789 240x240, SPI mode 3");

    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM initialization failed; refusing reduced graphics mode");
        return;
    }
    ESP_LOGI(TAG, "PSRAM: %u bytes, free: %u bytes", (unsigned)esp_psram_get_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    uint16_t *frames[2] = {
        heap_caps_aligned_alloc(64, MERRY_LCD_FRAME_BYTES,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        heap_caps_aligned_alloc(64, MERRY_LCD_FRAME_BYTES,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
    };
    if (frames[0] == NULL || frames[1] == NULL) {
        ESP_LOGE(TAG, "Could not allocate two full RGB565 PSRAM framebuffers");
        return;
    }
    ESP_LOGI(TAG, "Allocated two %u-byte RGB565 framebuffers in PSRAM",
             (unsigned)MERRY_LCD_FRAME_BYTES);

    transfer_done = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(transfer_done != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    backlight_init();
    esp_lcd_panel_handle_t panel = display_init();

    render_calibration(frames[0]);
    const int64_t calibration_us = flush_frame(panel, frames[0]);
    ESP_LOGI(TAG, "Calibration frame transfer: %.2f ms", calibration_us / 1000.0);
    vTaskDelay(pdMS_TO_TICKS(3000));

    uint32_t frame_number = 0;
    int64_t sample_started = esp_timer_get_time();
    int64_t slowest_transfer_us = 0;
    while (true) {
        uint16_t *frame = frames[frame_number & 1u];
        render_animation(frame, frame_number);
        const int64_t transfer_us = flush_frame(panel, frame);
        if (transfer_us > slowest_transfer_us) {
            slowest_transfer_us = transfer_us;
        }
        ++frame_number;

        if ((frame_number % 120u) == 0u) {
            const int64_t now = esp_timer_get_time();
            const double fps = 120000000.0 / (double)(now - sample_started);
            ESP_LOGI(TAG,
                     "FPS %.1f, slowest transfer %.2f ms, PSRAM free %u, internal DMA free %u",
                     fps, slowest_transfer_us / 1000.0,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
            sample_started = now;
            slowest_transfer_us = 0;
        }
    }
}
