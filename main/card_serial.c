#include "card_serial.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "card_display.h"
#include "card_storage.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wasm_plugin.h"

#define SERIAL_RX_BUFFER 4096
#define SERIAL_TX_BUFFER 2048
#define SERIAL_HEADER_MAX 128
#define SERIAL_PAYLOAD_IDLE_TIMEOUT_MS 30000
#define SERIAL_DRAIN_IDLE_TIMEOUT_MS 1000
#define SERIAL_TASK_STACK 8192

static const char *TAG = "card_serial";

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length)
{
    crc = ~crc;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

static void write_all(const char *data, size_t length)
{
    while (length > 0) {
        int written = usb_serial_jtag_write_bytes(data, length, pdMS_TO_TICKS(1000));
        if (written <= 0) {
            continue;
        }
        data += written;
        length -= (size_t)written;
    }
}

static void respond(uint32_t request_id, bool ok, const char *message)
{
    char safe[180];
    strlcpy(safe, message != NULL ? message : "", sizeof(safe));
    for (char *p = safe; *p != '\0'; ++p) {
        if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) {
            *p = '\'';
        }
    }
    char line[280];
    int length = snprintf(line, sizeof(line), "@CARD %lu %s {\"message\":\"%s\"}\n",
                          (unsigned long)request_id, ok ? "OK" : "ERR", safe);
    if (length > 0) {
        write_all(line, (size_t)length);
    }
}

static void respond_stage_error(uint32_t request_id, const char *stage, esp_err_t err)
{
    char message[180];
    snprintf(message, sizeof(message), "%s failed: %s", stage, esp_err_to_name(err));
    ESP_LOGE(TAG, "%s", message);
    respond(request_id, false, message);
}

static void respond_status(uint32_t request_id)
{
    char plugin_message[160];
    strlcpy(plugin_message, wasm_plugin_last_error(), sizeof(plugin_message));
    for (char *p = plugin_message; *p != '\0'; ++p) {
        if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) {
            *p = '\'';
        }
    }
    char line[512];
    int length = snprintf(
        line, sizeof(line),
        "@CARD %lu OK {\"message\":\"Device ready\",\"image\":%s,\"plugin\":%s,"
        "\"pluginMessage\":\"%s\",\"internalFree\":%u,\"psramFree\":%u}\n",
        (unsigned long)request_id,
        card_storage_file_size(CARD_IMAGE_PATH) == CARD_RGB565_BYTES ? "true" : "false",
        wasm_plugin_is_loaded() ? "true" : "false", plugin_message,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (length > 0) {
        write_all(line, (size_t)length);
    }
}

static bool read_header(char *header, size_t capacity)
{
    size_t used = 0;
    while (true) {
        uint8_t value;
        if (usb_serial_jtag_read_bytes(&value, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (value == '\n') {
            header[used] = '\0';
            return used > 0;
        }
        if (value == '\r') {
            continue;
        }
        if (used + 1 < capacity) {
            header[used++] = (char)value;
        } else {
            used = 0;
        }
    }
}

/* Reads and discards `length` payload bytes so the next header starts at a
   line boundary. Stops early only if the host goes quiet. */
static void drain_payload(size_t length)
{
    uint8_t buffer[256];
    while (length > 0) {
        const size_t wanted = length < sizeof(buffer) ? length : sizeof(buffer);
        const int received = usb_serial_jtag_read_bytes(
            buffer, wanted, pdMS_TO_TICKS(SERIAL_DRAIN_IDLE_TIMEOUT_MS));
        if (received <= 0) {
            return;
        }
        length -= (size_t)received;
    }
}

/* Always consumes the full payload from the link (unless the host stops
   sending), even when the file cannot be written, so a failed upload never
   leaves binary data in the stream to be parsed as headers. */
static esp_err_t receive_to_file(const char *path, size_t length, uint32_t expected_crc)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Cannot open %s for writing", path);
        drain_payload(length);
        return ESP_FAIL;
    }
    uint8_t buffer[2048];
    size_t remaining = length;
    uint32_t crc = 0;
    esp_err_t result = ESP_OK;
    while (remaining > 0) {
        size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        int received = usb_serial_jtag_read_bytes(
            buffer, wanted, pdMS_TO_TICKS(SERIAL_PAYLOAD_IDLE_TIMEOUT_MS));
        if (received <= 0) {
            ESP_LOGE(TAG, "Payload receive timed out with %u of %u bytes remaining",
                     (unsigned)remaining, (unsigned)length);
            fclose(file);
            remove(path);
            return ESP_ERR_TIMEOUT;
        }
        if (result == ESP_OK &&
            fwrite(buffer, 1, (size_t)received, file) != (size_t)received) {
            ESP_LOGE(TAG, "Payload file write failed (storage full?)");
            result = ESP_FAIL; /* keep reading so the stream stays in sync */
        }
        crc = crc32_update(crc, buffer, (size_t)received);
        remaining -= (size_t)received;
    }
    /* LittleFS may only hit the flash (and report "no space") on flush/close. */
    if (fflush(file) != 0) {
        result = ESP_FAIL;
    }
    if (fclose(file) != 0) {
        result = ESP_FAIL;
    }
    if (result != ESP_OK) {
        remove(path);
        return result;
    }
    if (crc != expected_crc) {
        remove(path);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

static void serial_task(void *argument)
{
    (void)argument;
    char header[SERIAL_HEADER_MAX];
    while (true) {
        if (!read_header(header, sizeof(header))) {
            continue;
        }
        unsigned long request_id = 0;
        unsigned long payload_length = 0;
        unsigned long payload_crc = 0;
        char command[20] = {0};
        if (sscanf(header, "CARD/1 %lu %19s %lu %lx", &request_id, command,
                   &payload_length, &payload_crc) != 4) {
            continue;
        }

        if (!card_storage_content_lock(5000)) {
            drain_payload((size_t)payload_length);
            respond((uint32_t)request_id, false, "Device busy applying wireless content");
            continue;
        }
        if (strcmp(command, "STATUS") == 0 && payload_length == 0) {
            respond_status((uint32_t)request_id);
        } else if (strcmp(command, "PUT_IMAGE") == 0 &&
                   payload_length == CARD_RGB565_BYTES) {
            esp_err_t err = receive_to_file(CARD_IMAGE_TEMP_PATH, payload_length,
                                             (uint32_t)payload_crc);
            const char *stage = "Image receive";
            if (err == ESP_OK) {
                err = card_storage_replace(CARD_IMAGE_TEMP_PATH, CARD_IMAGE_PATH);
                stage = "Image save";
            }
            if (err == ESP_OK) {
                err = card_display_show_rgb565_file(CARD_IMAGE_PATH);
                stage = "Image display";
            }
            if (err == ESP_OK && wasm_plugin_is_loaded()) {
                err = wasm_plugin_render();
                stage = "Plugin render";
            }
            if (err == ESP_OK) {
                respond((uint32_t)request_id, true, "Image saved and displayed");
            } else {
                respond_stage_error((uint32_t)request_id, stage, err);
            }
        } else if (strcmp(command, "PUT_PLUGIN") == 0 && payload_length >= 8 &&
                   payload_length <= CARD_PLUGIN_MAX_BYTES) {
            esp_err_t err = receive_to_file(CARD_PLUGIN_TEMP_PATH, payload_length,
                                             (uint32_t)payload_crc);
            const char *stage = "Plugin receive";
            if (err == ESP_OK) {
                err = wasm_plugin_load_and_run(CARD_PLUGIN_TEMP_PATH);
                stage = "Plugin load";
            }
            if (err == ESP_OK) {
                err = card_storage_replace(CARD_PLUGIN_TEMP_PATH, CARD_PLUGIN_PATH);
                stage = "Plugin save";
            } else {
                remove(CARD_PLUGIN_TEMP_PATH);
            }
            if (err == ESP_OK) {
                respond((uint32_t)request_id, true, "Plugin saved and activated");
            } else if (strcmp(stage, "Plugin load") == 0) {
                respond((uint32_t)request_id, false, wasm_plugin_last_error());
            } else {
                respond_stage_error((uint32_t)request_id, stage, err);
            }
        } else if (strcmp(command, "RENDER") == 0 && payload_length == 0) {
            esp_err_t err = wasm_plugin_render();
            respond((uint32_t)request_id, err == ESP_OK,
                    err == ESP_OK ? "Plugin rendered" : wasm_plugin_last_error());
        } else if (strcmp(command, "RESET_CONTENT") == 0 && payload_length == 0) {
            remove(CARD_IMAGE_TEMP_PATH);
            remove(CARD_PLUGIN_TEMP_PATH);
            remove(CARD_IMAGE_PATH);
            remove(CARD_PLUGIN_PATH);
            remove(CARD_PLUGIN_QUARANTINE_PATH);
            respond((uint32_t)request_id, true, "Saved content cleared; device restarting");
            vTaskDelay(pdMS_TO_TICKS(250));
            esp_restart();
        } else {
            /* Unknown command or rejected size: skip whatever payload the host
               is about to send so the stream stays aligned on headers. */
            drain_payload((size_t)payload_length);
            respond((uint32_t)request_id, false, "Invalid command or payload size");
        }
        card_storage_content_unlock();
    }
}

esp_err_t card_serial_start(void)
{
    const usb_serial_jtag_driver_config_t config = {
        .rx_buffer_size = SERIAL_RX_BUFFER,
        .tx_buffer_size = SERIAL_TX_BUFFER,
    };
    esp_err_t err = usb_serial_jtag_driver_install((usb_serial_jtag_driver_config_t *)&config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    usb_serial_jtag_vfs_use_driver();
    if (xTaskCreate(serial_task, "card_serial", SERIAL_TASK_STACK, NULL, 8, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "CARD/1 Web Serial protocol ready on native USB");
    return ESP_OK;
}
