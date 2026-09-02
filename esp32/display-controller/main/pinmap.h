/* SPDX-License-Identifier: MIT */

#pragma once

/* Regular Seeed XIAO ESP32-S3 castellated-pin mapping. */

/* Dedicated high-speed ST7789 SPI master (SPI2). */
#define MERRY_TFT_PIN_DC 1       /* D0 */
#define MERRY_TFT_PIN_RESET 2    /* D1 */
#define MERRY_TFT_PIN_BACKLIGHT 4 /* D3 */
#define MERRY_TFT_PIN_SCLK 7     /* D8 */
#define MERRY_TFT_PIN_MOSI 9     /* D10 */

/* Low-speed, noise-tolerant keyboard status UART. The links are crossed:
 * nRF D4 TX -> ESP D4 RX, ESP D5 TX -> nRF D5 RX. */
#define MERRY_KEYBOARD_UART_RX 5 /* D4 */
#define MERRY_KEYBOARD_UART_TX 6 /* D5 */

/* D2 / GPIO3 remains unused because it is an ESP32-S3 strapping pin. */
#define MERRY_SPARE_PIN 3
