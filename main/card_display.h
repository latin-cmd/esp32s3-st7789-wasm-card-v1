#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#define CARD_SCREEN_WIDTH 240
#define CARD_SCREEN_HEIGHT 280
#define CARD_RGB565_BYTES ((size_t)CARD_SCREEN_WIDTH * CARD_SCREEN_HEIGHT * sizeof(uint16_t))

/* Replaces the whole screen with LVGL objects created by `build` (called with
   the LVGL lock held) and releases any image buffer shown before. */
typedef void (*card_display_builder_t)(lv_obj_t *screen);
esp_err_t card_display_show_builder(card_display_builder_t build);

esp_err_t card_display_show_rgb565_file(const char *path);
esp_err_t card_display_show_rgb565_copy(const uint16_t *pixels, size_t byte_count);
const uint16_t *card_display_current_image(void);
bool card_display_has_image(void);
