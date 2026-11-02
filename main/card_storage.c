#include "card_storage.h"

#include <stdio.h>
#include <sys/stat.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "card_storage";
static bool s_mounted;
static SemaphoreHandle_t s_content_lock;

bool card_storage_content_lock(uint32_t timeout_ms)
{
    return s_content_lock && xSemaphoreTake(s_content_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void card_storage_content_unlock(void)
{
    if (s_content_lock) xSemaphoreGive(s_content_lock);
}

esp_err_t card_storage_init(void)
{
    if (!s_content_lock) s_content_lock = xSemaphoreCreateMutex();
    if (!s_content_lock) return ESP_ERR_NO_MEM;
    const esp_vfs_littlefs_conf_t conf = {
        .base_path = CARD_STORAGE_BASE_PATH,
        .partition_label = "storage",
        .partition = NULL,
        .format_if_mount_failed = true,
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = true,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_mounted = true;

    size_t total = 0;
    size_t used = 0;
    err = esp_littlefs_info("storage", &total, &used);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS ready: %u KB total, %u KB used",
                 (unsigned)(total / 1024), (unsigned)(used / 1024));
    }

    /* Leftovers from an upload interrupted by a reset are never valid. */
    remove(CARD_IMAGE_TEMP_PATH);
    remove(CARD_PLUGIN_TEMP_PATH);
    remove(CARD_STORAGE_BASE_PATH "/wireless.rgb.tmp");
    return ESP_OK;
}

bool card_storage_is_mounted(void)
{
    return s_mounted;
}

bool card_storage_file_exists(const char *path)
{
    struct stat st;
    return path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

size_t card_storage_file_size(const char *path)
{
    struct stat st;
    if (path == NULL || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return 0;
    }
    return (size_t)st.st_size;
}

esp_err_t card_storage_replace(const char *temp_path, const char *final_path)
{
    if (temp_path == NULL || final_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* LittleFS rename atomically replaces an existing destination, so a power
       cut leaves either the old or the new file, never neither. */
    if (rename(temp_path, final_path) != 0) {
        ESP_LOGE(TAG, "Failed to replace %s", final_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}
