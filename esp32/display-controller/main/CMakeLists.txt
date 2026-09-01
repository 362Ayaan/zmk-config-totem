idf_component_register(
    SRCS "main.c" "merry_runtime.c" "merry_protocol.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd esp_psram esp_timer spiffs
)
