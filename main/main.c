/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "card_display.h"
#include "card_storage.h"
#include "card_serial.h"
#include "wasm_plugin.h"
/* LVGL is configured through Kconfig (CONFIG_LV_CONF_SKIP=y), so main/lv_conf.h
 * is intentionally NOT included here: mixing it in would redefine LV_* macros
 * for this file only. lv_demos.h is no longer needed either. */
#include "esp_lcd_touch_cst816s.h"
/* LCD size */
#define EXAMPLE_LCD_H_RES (240)
#define EXAMPLE_LCD_V_RES (280)

/* LCD settings */
#define EXAMPLE_LCD_SPI_NUM (SPI2_HOST)
#define EXAMPLE_LCD_PIXEL_CLK_HZ (40 * 1000 * 1000)
#define EXAMPLE_LCD_CMD_BITS (8)
#define EXAMPLE_LCD_PARAM_BITS (8)
#define EXAMPLE_LCD_COLOR_SPACE (ESP_LCD_COLOR_SPACE_RGB)
#define EXAMPLE_LCD_BITS_PER_PIXEL (16)
#define EXAMPLE_LCD_DRAW_BUFF_DOUBLE (1)
#define EXAMPLE_LCD_DRAW_BUFF_HEIGHT (50)
#define EXAMPLE_LCD_BL_ON_LEVEL (1)

/* LCD pins */
#define EXAMPLE_LCD_GPIO_SCLK (GPIO_NUM_6)
#define EXAMPLE_LCD_GPIO_MOSI (GPIO_NUM_7)
#define EXAMPLE_LCD_GPIO_RST (GPIO_NUM_8)
#define EXAMPLE_LCD_GPIO_DC (GPIO_NUM_4)
#define EXAMPLE_LCD_GPIO_CS (GPIO_NUM_5)
#define EXAMPLE_LCD_GPIO_BL (GPIO_NUM_15)

#define EXAMPLE_USE_TOUCH 0

#define TOUCH_HOST I2C_NUM_0

#if EXAMPLE_USE_TOUCH
#define EXAMPLE_PIN_NUM_TOUCH_SCL (GPIO_NUM_10)
#define EXAMPLE_PIN_NUM_TOUCH_SDA (GPIO_NUM_11)
#define EXAMPLE_PIN_NUM_TOUCH_RST (GPIO_NUM_13)
#define EXAMPLE_PIN_NUM_TOUCH_INT (GPIO_NUM_14)

esp_lcd_touch_handle_t tp = NULL;
#endif

static const char *TAG = "EXAMPLE";

/* ======================== Business card content ========================
 * Edit the strings below to change what the card shows.
 * - ASCII text renders with Montserrat 14/16 (built into LVGL via Kconfig)
 *   or with the generated Noto Sans SC fonts in main/font_card_*.c.
 * - font_card_cjk_16 only contains printable ASCII, U+00B7 and the Chinese
 *   characters of CARD_COMPANY_CN. If you change any Chinese text, regenerate
 *   it (command in the header of main/font_card_cjk_16.c), otherwise the new
 *   characters will show as blanks.
 * ===================================================================== */
#define CARD_NAME        "Electronic Card"
#define CARD_INITIALS    ""
#define CARD_TITLE       "Ready to customize"
#define CARD_COMPANY_CN  ""
#define CARD_COMPANY_EN  ""
#define CARD_PHONE       ""
#define CARD_EMAIL       ""
#define CARD_WEB         ""
#define CARD_SLOGAN      "240 x 280 RGB565"

/* Generated fonts (Noto Sans SC, 4 bpp, subset) */
LV_FONT_DECLARE(font_card_name_24);   /* Bold 24 px, ASCII 0x20-0x7E */
LV_FONT_DECLARE(font_card_cjk_16);    /* Medium 16 px, ASCII + U+00B7 + company Hanzi */

/* Palette: charcoal-navy background, teal primary accent, amber secondary */
#define CARD_COLOR_BG_TOP     0x0A1120
#define CARD_COLOR_BG_BOTTOM  0x16223B
#define CARD_COLOR_PANEL      0x1A2742
#define CARD_COLOR_BORDER     0x2B3C60
#define CARD_COLOR_ACCENT     0x2DD4BF
#define CARD_COLOR_ACCENT_2   0xF5B544
#define CARD_COLOR_AVATAR_END 0x3B82F6
#define CARD_COLOR_TEXT       0xF1F5F9
#define CARD_COLOR_MUTED      0x94A3B8
#define CARD_COLOR_INK        0x0A1120

/* Layout (portrait 240 x 280). All y values are offsets from the top edge. */
#define CARD_PANEL_W     (224)
#define CARD_AVATAR_D    (56)
#define CARD_AVATAR_Y    (7)
#define CARD_NAME_Y      (64)
#define CARD_TITLE_Y     (93)
#define CARD_BAR_Y       (116)
#define CARD_COMPANY_Y   (124)
#define CARD_COMPANY_H   (44)
#define CARD_INFO_Y      (173)
#define CARD_INFO_ROW_H  (26)
#define CARD_INFO_H      (3 * CARD_INFO_ROW_H + 4)
#define CARD_SLOGAN_Y    (258)

/* LCD IO and panel */
static esp_lcd_panel_io_handle_t lcd_io = NULL;
static esp_lcd_panel_handle_t lcd_panel = NULL;

/* LVGL display and touch */
static lv_display_t *lvgl_disp = NULL;

static esp_err_t app_lcd_init(void)
{
    esp_err_t ret = ESP_OK;

    /* LCD backlight */
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << EXAMPLE_LCD_GPIO_BL};
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));

    /* LCD initialization */
    ESP_LOGD(TAG, "Initialize SPI bus");
    const spi_bus_config_t buscfg = {
        .sclk_io_num = EXAMPLE_LCD_GPIO_SCLK,
        .mosi_io_num = EXAMPLE_LCD_GPIO_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = EXAMPLE_LCD_H_RES * EXAMPLE_LCD_DRAW_BUFF_HEIGHT * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(EXAMPLE_LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO), TAG, "SPI init failed");

    ESP_LOGD(TAG, "Install panel IO");
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = EXAMPLE_LCD_GPIO_DC,
        .cs_gpio_num = EXAMPLE_LCD_GPIO_CS,
        .pclk_hz = EXAMPLE_LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits = EXAMPLE_LCD_CMD_BITS,
        .lcd_param_bits = EXAMPLE_LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)EXAMPLE_LCD_SPI_NUM, &io_config, &lcd_io), err, TAG, "New panel IO failed");

    ESP_LOGD(TAG, "Install LCD driver");
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EXAMPLE_LCD_GPIO_RST,
        .color_space = EXAMPLE_LCD_COLOR_SPACE,
        .bits_per_pixel = EXAMPLE_LCD_BITS_PER_PIXEL,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st7789(lcd_io, &panel_config, &lcd_panel), err, TAG, "New panel failed");

    esp_lcd_panel_reset(lcd_panel);
    esp_lcd_panel_init(lcd_panel);
    esp_lcd_panel_mirror(lcd_panel, true, true);
    esp_lcd_panel_disp_on_off(lcd_panel, true);

    /* LCD backlight on */
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_LCD_GPIO_BL, EXAMPLE_LCD_BL_ON_LEVEL));

    esp_lcd_panel_set_gap(lcd_panel, 0, 20);
    esp_lcd_panel_invert_color(lcd_panel, true);

    return ret;

err:
    if (lcd_panel)
    {
        esp_lcd_panel_del(lcd_panel);
    }
    if (lcd_io)
    {
        esp_lcd_panel_io_del(lcd_io);
    }
    spi_bus_free(EXAMPLE_LCD_SPI_NUM);
    return ret;
}

#if EXAMPLE_USE_TOUCH
static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)drv->user_data;
    assert(tp);

    uint16_t tp_x;
    uint16_t tp_y;
    uint8_t tp_cnt = 0;
    /* Read data from touch controller into memory */
    esp_lcd_touch_read_data(tp);
    /* Read data from touch controller */
    bool tp_pressed = esp_lcd_touch_get_coordinates(tp, &tp_x, &tp_y, NULL, &tp_cnt, 1);
    if (tp_pressed && tp_cnt > 0)
    {
        data->point.x = tp_x;
        data->point.y = tp_y;
        data->state = LV_INDEV_STATE_PRESSED;
        ESP_LOGD(TAG, "Touch position: %d,%d", tp_x, tp_y);
    }
    else
    {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
#endif

static esp_err_t app_lvgl_init(void)
{
    /* Initialize LVGL */
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,       /* LVGL task priority */
        .task_stack = 4096,       /* LVGL task stack size */
        .task_affinity = -1,      /* LVGL task pinned to core (-1 is no affinity) */
        .task_max_sleep_ms = 500, /* Maximum sleep in LVGL task */
        .timer_period_ms = 5      /* LVGL timer tick period in ms */
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port initialization failed");

    /* Add LCD screen */
    ESP_LOGD(TAG, "Add LCD screen");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd_io,
        .panel_handle = lcd_panel,
        .buffer_size = EXAMPLE_LCD_H_RES * EXAMPLE_LCD_DRAW_BUFF_HEIGHT * sizeof(uint16_t),
        .double_buffer = EXAMPLE_LCD_DRAW_BUFF_DOUBLE,
        .hres = EXAMPLE_LCD_H_RES,
        .vres = EXAMPLE_LCD_V_RES,
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
        }};
    lvgl_disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(lvgl_disp != NULL, ESP_FAIL, TAG, "LVGL add display failed");

    return ESP_OK;
}

/* ============================ UI helpers ============================ */

/* Strip theme styles and make an object purely decorative (static card). */
static void card_obj_reset(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

static lv_obj_t *card_create_label(lv_obj_t *parent, const char *text,
                                   const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text); /* LVGL keeps its own copy of the text */
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

/* Single-line, horizontally centred label that can never exceed `width`. */
static lv_obj_t *card_create_centered_label(lv_obj_t *parent, const char *text,
                                            const lv_font_t *font, uint32_t color,
                                            lv_coord_t width, lv_coord_t y)
{
    lv_obj_t *label = card_create_label(parent, text, font, color);
    /* Fixed one-line box: LONG_DOT truncates with "..." instead of wrapping */
    lv_obj_set_size(label, width, lv_font_get_line_height(font));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

static lv_obj_t *card_create_circle(lv_obj_t *parent, lv_coord_t d, uint32_t color, lv_opa_t opa)
{
    lv_obj_t *c = lv_obj_create(parent);
    card_obj_reset(c);
    lv_obj_set_size(c, d, d);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(c, opa, 0);
    return c;
}

static lv_obj_t *card_create_panel(lv_obj_t *parent, lv_coord_t y, lv_coord_t h)
{
    lv_obj_t *panel = lv_obj_create(parent);
    card_obj_reset(panel);
    lv_obj_set_size(panel, CARD_PANEL_W, h);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(CARD_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(CARD_COLOR_BORDER), 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    return panel;
}

static void card_create_hline(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                              lv_coord_t h, uint32_t color)
{
    lv_obj_t *line = lv_obj_create(parent);
    card_obj_reset(line);
    lv_obj_set_size(line, w, h);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_style_radius(line, h / 2, 0);
    lv_obj_set_style_bg_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
}

/* One contact row: round icon badge + single-line text clipped with "..." */
static void card_add_info_row(lv_obj_t *panel, uint8_t row, const char *symbol, const char *text)
{
    const lv_coord_t badge_x = 10;
    const lv_coord_t badge_d = 20;
    const lv_coord_t text_x = badge_x + badge_d + 8;
    const lv_coord_t text_w = CARD_PANEL_W - text_x - 8;
    const lv_font_t *font = &lv_font_montserrat_14;
    const lv_coord_t y = 2 + row * CARD_INFO_ROW_H + (CARD_INFO_ROW_H - badge_d) / 2;

    lv_obj_t *badge = card_create_circle(panel, badge_d, CARD_COLOR_ACCENT, LV_OPA_20);
    lv_obj_set_pos(badge, badge_x, y);

    lv_obj_t *icon = card_create_label(badge, symbol, &lv_font_montserrat_14, CARD_COLOR_ACCENT);
    lv_obj_center(icon);

    lv_obj_t *label = card_create_label(panel, text, font, CARD_COLOR_TEXT);
    lv_obj_set_size(label, text_w, lv_font_get_line_height(font)); /* one line, "..." if too long */
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(label, text_x, y + (badge_d - lv_font_get_line_height(font)) / 2);

    if (row > 0) {
        /* hairline divider above this row, aligned with the text column */
        card_create_hline(panel, text_x, 2 + row * CARD_INFO_ROW_H - 1,
                          text_w, 1, CARD_COLOR_BORDER);
    }
}

static void card_build(lv_obj_t *scr)
{
    /* ---- Background: vertical gradient + two soft decorative circles ---- */
    lv_obj_clean(scr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(CARD_COLOR_BG_TOP), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(CARD_COLOR_BG_BOTTOM), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *deco1 = card_create_circle(scr, 160, CARD_COLOR_ACCENT, LV_OPA_10);
    lv_obj_set_pos(deco1, 130, -70);
    lv_obj_t *deco2 = card_create_circle(scr, 120, CARD_COLOR_ACCENT_2, LV_OPA_10);
    lv_obj_set_pos(deco2, -60, 70);

    /* ---- Avatar: gradient circle with initials and a soft ring ---- */
    lv_obj_t *avatar = card_create_circle(scr, CARD_AVATAR_D, CARD_COLOR_ACCENT, LV_OPA_COVER);
    lv_obj_set_style_bg_grad_color(avatar, lv_color_hex(CARD_COLOR_AVATAR_END), 0);
    lv_obj_set_style_bg_grad_dir(avatar, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_outline_color(avatar, lv_color_hex(CARD_COLOR_ACCENT), 0);
    lv_obj_set_style_outline_opa(avatar, LV_OPA_40, 0);
    lv_obj_set_style_outline_width(avatar, 2, 0);
    lv_obj_set_style_outline_pad(avatar, 3, 0);
    lv_obj_align(avatar, LV_ALIGN_TOP_MID, 0, CARD_AVATAR_Y);

    lv_obj_t *initials = card_create_label(avatar, CARD_INITIALS, &font_card_name_24, CARD_COLOR_INK);
    lv_obj_center(initials);

    /* ---- Identity: name, title, accent bar ---- */
    card_create_centered_label(scr, CARD_NAME, &font_card_name_24, CARD_COLOR_TEXT,
                               CARD_PANEL_W, CARD_NAME_Y);
    card_create_centered_label(scr, CARD_TITLE, &lv_font_montserrat_16, CARD_COLOR_ACCENT,
                               CARD_PANEL_W, CARD_TITLE_Y);

    lv_obj_t *bar = lv_obj_create(scr);
    card_obj_reset(bar);
    lv_obj_set_size(bar, 32, 3);
    lv_obj_set_style_radius(bar, 1, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(CARD_COLOR_ACCENT_2), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, CARD_BAR_Y);

    /* ---- Company card: teal edge on the left, CN + EN names ---- */
    lv_obj_t *company = card_create_panel(scr, CARD_COMPANY_Y, CARD_COMPANY_H);
    lv_obj_set_style_border_color(company, lv_color_hex(CARD_COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(company, 3, 0);
    lv_obj_set_style_border_side(company, LV_BORDER_SIDE_LEFT, 0);

    card_create_centered_label(company, CARD_COMPANY_CN, &font_card_cjk_16, CARD_COLOR_TEXT,
                               CARD_PANEL_W - 16, 3);
    card_create_centered_label(company, CARD_COMPANY_EN, &lv_font_montserrat_14, CARD_COLOR_MUTED,
                               CARD_PANEL_W - 16, 3 + lv_font_get_line_height(&font_card_cjk_16) + 2);

    /* ---- Contact card ---- */
    lv_obj_t *info = card_create_panel(scr, CARD_INFO_Y, CARD_INFO_H);
    card_add_info_row(info, 0, LV_SYMBOL_CALL, CARD_PHONE);
    card_add_info_row(info, 1, LV_SYMBOL_ENVELOPE, CARD_EMAIL);
    card_add_info_row(info, 2, LV_SYMBOL_HOME, CARD_WEB);

    /* ---- Slogan ---- */
    card_create_centered_label(scr, CARD_SLOGAN, &font_card_cjk_16, CARD_COLOR_ACCENT_2,
                               CARD_PANEL_W - 16, CARD_SLOGAN_Y);
}

static void app_main_display(void)
{
    /* card_display_show_builder takes the LVGL lock, clears the screen and
       frees any uploaded image buffer before building the static card. */
    esp_err_t err = card_display_show_builder(card_build);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Static card display failed: %s", esp_err_to_name(err));
    }
}

/* ---------------------- Content restore / safe mode ---------------------- */

#define SAFE_MODE_GPIO        GPIO_NUM_0   /* BOOT button, active low */
#define NVS_NAMESPACE         "card"
#define NVS_KEY_PLUGIN_BOOT   "plg_boot"   /* 1 while a boot-time plugin load runs */

static bool s_nvs_ok;

/* Shows the saved image if it is valid, otherwise the built-in card. Used at
   boot and by wasm_plugin when a rejected plugin already drew on screen. */
static void show_default_content(void)
{
    if (card_storage_file_size(CARD_IMAGE_PATH) == CARD_RGB565_BYTES &&
        card_display_show_rgb565_file(CARD_IMAGE_PATH) == ESP_OK) {
        return;
    }
    app_main_display();
}

static bool boot_button_held(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << SAFE_MODE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    return gpio_get_level(SAFE_MODE_GPIO) == 0;
}

static void plugin_boot_flag_set(uint8_t value)
{
    if (!s_nvs_ok) {
        return;
    }
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY_PLUGIN_BOOT, value);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static bool plugin_boot_flag_get(void)
{
    uint8_t value = 0;
    nvs_handle_t nvs;
    if (s_nvs_ok && nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, NVS_KEY_PLUGIN_BOOT, &value);
        nvs_close(nvs);
    }
    return value != 0;
}

/* Restores the saved plugin unless the previous attempt never finished
   (crash, watchdog or power-cycle while it ran): that plugin is quarantined so
   the device cannot get stuck in a reboot loop. */
static bool restore_saved_plugin(bool safe_mode)
{
    if (!card_storage_file_exists(CARD_PLUGIN_PATH)) {
        return false;
    }
    if (safe_mode) {
        ESP_LOGW(TAG, "BOOT button held: skipping saved plugin");
        return false;
    }
    if (plugin_boot_flag_get()) {
        ESP_LOGE(TAG, "Saved plugin did not finish loading last boot; quarantining it");
        remove(CARD_PLUGIN_QUARANTINE_PATH);
        if (rename(CARD_PLUGIN_PATH, CARD_PLUGIN_QUARANTINE_PATH) != 0) {
            remove(CARD_PLUGIN_PATH);
        }
        plugin_boot_flag_set(0);
        return false;
    }

    plugin_boot_flag_set(1);
    esp_err_t err = wasm_plugin_load_and_run(CARD_PLUGIN_PATH);
    plugin_boot_flag_set(0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Saved plugin rejected: %s", wasm_plugin_last_error());
        return false;
    }
    return true;
}

void app_main(void)
{
    /* LCD HW initialization */
    ESP_ERROR_CHECK(app_lcd_init());

#if EXAMPLE_USE_TOUCH
    ESP_LOGI(TAG, "Initialize I2C bus");
    esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_NONE);
    esp_log_level_set("CST816S", ESP_LOG_NONE);
    const i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = EXAMPLE_PIN_NUM_TOUCH_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = EXAMPLE_PIN_NUM_TOUCH_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100 * 1000,
    };
    i2c_param_config(TOUCH_HOST, &i2c_conf);

    i2c_driver_install(TOUCH_HOST, i2c_conf.mode, 0, 0, 0);

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    const esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    // Attach the TOUCH to the I2C bus
    esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)TOUCH_HOST, &tp_io_config, &tp_io_handle);

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = EXAMPLE_LCD_H_RES,
        .y_max = EXAMPLE_LCD_V_RES,
        .rst_gpio_num = EXAMPLE_PIN_NUM_TOUCH_RST,
        .int_gpio_num = EXAMPLE_PIN_NUM_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    ESP_LOGI(TAG, "Initialize touch controller");
    esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, &tp);
#endif

    /* LVGL initialization */
    ESP_ERROR_CHECK(app_lvgl_init());

#if EXAMPLE_USE_TOUCH
    static lv_indev_drv_t indev_drv; // Input device driver (Touch)
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.disp = lvgl_disp;
    indev_drv.read_cb = example_lvgl_touch_cb;
    indev_drv.user_data = tp;
    lv_indev_drv_register(&indev_drv);
#endif

    /* Show the built-in card right away; saved content replaces it below. */
    app_main_display();

    ESP_LOGI(TAG, "PSRAM: %u bytes total, %u bytes free",
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    const bool safe_mode = boot_button_held();

    /* Nothing below is fatal: on any failure the built-in card stays up. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    s_nvs_ok = err == ESP_OK;
    if (!s_nvs_ok) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
    }

    err = card_storage_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Storage unavailable, keeping the built-in card: %s", esp_err_to_name(err));
    }
    const bool plugins_ok = wasm_plugin_init() == ESP_OK;
    if (!plugins_ok) {
        ESP_LOGE(TAG, "Plugin runtime unavailable: %s", wasm_plugin_last_error());
    }
    wasm_plugin_set_fallback(show_default_content);

    /* Start the USB protocol before touching saved content, so a bad image or
       plugin can always be replaced or cleared (RESET_CONTENT) over USB. */
    err = card_serial_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB protocol failed to start: %s", esp_err_to_name(err));
    }

    if (!card_storage_is_mounted()) {
        return;
    }
    bool content_restored = plugins_ok && restore_saved_plugin(safe_mode);
    if (!content_restored && !safe_mode &&
        card_storage_file_size(CARD_IMAGE_PATH) == CARD_RGB565_BYTES) {
        err = card_display_show_rgb565_file(CARD_IMAGE_PATH);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Saved image could not be displayed: %s", esp_err_to_name(err));
        }
    }
}
