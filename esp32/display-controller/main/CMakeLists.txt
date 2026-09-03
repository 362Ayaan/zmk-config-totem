idf_component_register(
    SRCS "main.c" "merry_runtime.c" "merry_protocol.c" "merry_keyboard_uart.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd esp_partition esp_psram esp_timer
)
