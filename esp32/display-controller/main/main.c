/* SPDX-License-Identifier: MIT */

#include <stdbool.h>
#include <stdint.h>

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "merry_protocol.h"
#include "merry_runtime.h"
#include "pinmap.h"

#define MERRY_LCD_SPI_HZ (40 * 1000 * 1000)

static const char *const TAG = "merry-display";
static SemaphoreHandle_t transfer_done;

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
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

static void backlight_set(bool on) {
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, on ? 1023 : 0));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
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

static void flush_frame(esp_lcd_panel_handle_t panel, uint16_t *frame) {
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, MERRY_LCD_WIDTH,
                                               MERRY_LCD_HEIGHT, frame));
    ESP_ERROR_CHECK(xSemaphoreTake(transfer_done, pdMS_TO_TICKS(1000)) == pdTRUE
                        ? ESP_OK
                        : ESP_ERR_TIMEOUT);
}

void app_main(void) {
    ESP_LOGI(TAG, "Merry ESP32-S3 full-colour runtime");
    if (!esp_psram_is_initialized() || esp_psram_get_size() < 8u * 1024u * 1024u) {
        ESP_LOGE(TAG, "8 MB PSRAM is required; refusing reduced graphics mode");
        return;
    }
    ESP_LOGI(TAG, "PSRAM before assets: %u bytes free",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    uint16_t *frames[2] = {
        heap_caps_aligned_alloc(64, MERRY_LCD_FRAME_BYTES,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        heap_caps_aligned_alloc(64, MERRY_LCD_FRAME_BYTES,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
    };
    if (frames[0] == NULL || frames[1] == NULL) {
        ESP_LOGE(TAG, "Could not allocate two RGB565 framebuffers");
        return;
    }
    ESP_ERROR_CHECK(merry_runtime_init());
    ESP_LOGI(TAG, "Full RGB565 Merry pack and atomic media buffers loaded");
    ESP_LOGI(TAG, "PSRAM after assets: %u bytes free",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    transfer_done = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(transfer_done != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    backlight_init();
    esp_lcd_panel_handle_t panel = display_init();
    merry_runtime_attach_renderer(xTaskGetCurrentTaskHandle());

    bool changed = false;
    bool requested_power = true;
    (void)merry_runtime_render(frames[0], &changed, &requested_power);
    if (changed) {
        flush_frame(panel, frames[0]);
    }
    bool screen_power = requested_power;
    backlight_set(screen_power);

    ESP_LOGI(TAG, "V1-compatible CRC/ACK serial protocol starting");
    /* Protocol and boot logs share native USB. Silence steady-state logs before
     * installing the protocol so acknowledgements cannot be interleaved with
     * diagnostic text. The bridge discards the finite boot log at handshake. */
    esp_log_level_set("*", ESP_LOG_NONE);
    ESP_ERROR_CHECK(merry_protocol_start());

    unsigned frame_index = 1;
    while (true) {
        uint16_t *frame = frames[frame_index & 1u];
        const uint32_t delay_ms =
            merry_runtime_render(frame, &changed, &requested_power);
        if (requested_power != screen_power) {
            if (requested_power) {
                ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
                backlight_set(true);
            } else {
                backlight_set(false);
                ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, false));
            }
            screen_power = requested_power;
        }
        if (screen_power && changed) {
            flush_frame(panel, frame);
            ++frame_index;
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms));
    }
}
