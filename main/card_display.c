#include "card_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "card_display";
static uint16_t *s_pixels;
static lv_img_dsc_t s_image;

static esp_err_t show_owned_buffer(uint16_t *pixels)
{
    if (pixels == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* The serial command task is allowed to wait for LVGL to finish its
       current frame.  A short timeout made valid full-screen uploads fail
       intermittently while the display task was busy. */
    if (!lvgl_port_lock(0)) {
        free(pixels);
        return ESP_FAIL;
    }

    lv_obj_t *screen = lv_scr_act();
    if (screen == NULL) {
        lvgl_port_unlock();
        free(pixels);
        return ESP_ERR_INVALID_STATE;
    }

    lv_obj_clean(screen);
    if (s_pixels != NULL) {
        free(s_pixels);
    }
    s_pixels = pixels;

    memset(&s_image, 0, sizeof(s_image));
    s_image.header.always_zero = 0;
    s_image.header.w = CARD_SCREEN_WIDTH;
    s_image.header.h = CARD_SCREEN_HEIGHT;
    s_image.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_image.data_size = CARD_RGB565_BYTES;
    s_image.data = (const uint8_t *)s_pixels;

    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_t *image = lv_img_create(screen);
    lv_img_set_src(image, &s_image);
    lv_obj_center(image);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "Displayed 240x280 RGB565 image");
    return ESP_OK;
}

esp_err_t card_display_show_rgb565_file(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    uint16_t *pixels = heap_caps_malloc(CARD_RGB565_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    const size_t read_count = fread(pixels, 1, CARD_RGB565_BYTES, file);
    const int extra = fgetc(file);
    fclose(file);
    if (read_count != CARD_RGB565_BYTES || extra != EOF) {
        free(pixels);
        ESP_LOGE(TAG, "Invalid RGB565 image size: %u bytes", (unsigned)read_count);
        return ESP_ERR_INVALID_SIZE;
    }
    return show_owned_buffer(pixels);
}

esp_err_t card_display_show_rgb565_copy(const uint16_t *pixels, size_t byte_count)
{
    if (pixels == NULL || byte_count != CARD_RGB565_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t *copy = heap_caps_malloc(CARD_RGB565_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, pixels, CARD_RGB565_BYTES);
    return show_owned_buffer(copy);
}

esp_err_t card_display_show_builder(card_display_builder_t build)
{
    if (build == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lvgl_port_lock(0)) {
        return ESP_FAIL;
    }
    lv_obj_t *screen = lv_scr_act();
    if (screen == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    /* Delete the image object first, then its pixel buffer. */
    lv_obj_clean(screen);
    free(s_pixels);
    s_pixels = NULL;
    build(screen);
    lvgl_port_unlock();
    return ESP_OK;
}

const uint16_t *card_display_current_image(void)
{
    return s_pixels;
}

bool card_display_has_image(void)
{
    return s_pixels != NULL;
}
