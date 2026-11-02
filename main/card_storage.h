#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define CARD_STORAGE_BASE_PATH "/data"
#define CARD_IMAGE_PATH CARD_STORAGE_BASE_PATH "/current.rgb"
#define CARD_IMAGE_TEMP_PATH CARD_STORAGE_BASE_PATH "/current.rgb.tmp"
#define CARD_PLUGIN_PATH CARD_STORAGE_BASE_PATH "/plugin.wasm"
#define CARD_PLUGIN_TEMP_PATH CARD_STORAGE_BASE_PATH "/plugin.wasm.tmp"
/* A plugin that crashed or hung the device during boot is moved here. */
#define CARD_PLUGIN_QUARANTINE_PATH CARD_STORAGE_BASE_PATH "/plugin.wasm.bad"

esp_err_t card_storage_init(void);
bool card_storage_is_mounted(void);
bool card_storage_file_exists(const char *path);
size_t card_storage_file_size(const char *path);
esp_err_t card_storage_replace(const char *temp_path, const char *final_path);

