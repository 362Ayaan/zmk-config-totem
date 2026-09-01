/* SPDX-License-Identifier: MIT */

#pragma once

/* Regular Seeed XIAO ESP32-S3 castellated-pin mapping. */

/* Dedicated high-speed ST7789 SPI master (SPI2). */
#define MERRY_TFT_PIN_DC 1       /* D0 */
#define MERRY_TFT_PIN_RESET 2    /* D1 */
#define MERRY_TFT_PIN_BACKLIGHT 4 /* D3 */
#define MERRY_TFT_PIN_SCLK 7     /* D8 */
#define MERRY_TFT_PIN_MOSI 9     /* D10 */

/* Future nRF52840-to-ESP32-S3 full-duplex SPI slave (SPI3). */
#define MERRY_LINK_PIN_CS 5      /* D4 */
#define MERRY_LINK_PIN_READY 6   /* D5 */
#define MERRY_LINK_PIN_SCLK 43   /* D6 */
#define MERRY_LINK_PIN_MOSI 44   /* D7: nRF -> ESP */
#define MERRY_LINK_PIN_MISO 8    /* D9: ESP -> nRF */

/* D2 / GPIO3 remains unused because it is an ESP32-S3 strapping pin. */
#define MERRY_SPARE_PIN 3
