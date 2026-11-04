#include "card_network.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_crt_bundle.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "card_storage.h"
#include "wasm_plugin.h"

#define WIFI_READY BIT0
#define WS_READY BIT1
#define IMAGE_TEMP CARD_STORAGE_BASE_PATH "/wireless.rgb.tmp"

static const char *TAG = "card_network";
static EventGroupHandle_t s_events;
static QueueHandle_t s_assignments;
static SemaphoreHandle_t s_image_lock;
static esp_netif_t *s_sta_netif;
static httpd_handle_t s_httpd;
static esp_mqtt_client_handle_t s_mqtt;
static esp_websocket_client_handle_t s_ws;
static card_display_builder_t s_fallback;
static bool s_ap_active, s_claimed, s_registered, s_manual_setup;
static volatile bool s_force_setup;
static char s_ssid[33], s_password[64], s_server[181];
static char s_device_id[24], s_secret[65], s_ap_name[32], s_ap_password[17];
static char s_claim_code[20], s_message[96] = "Connect phone to Card hotspot";
static char s_mqtt_uri[192], s_ws_uri[256], s_ws_headers[160];

typedef struct { char tenant[40]; char websocket_url[256]; } assignment_t;
typedef struct { char data[1536]; size_t used; bool overflow; } http_buffer_t;
typedef struct {
    FILE *file;
    size_t bytes;
    uint32_t crc, expected_crc;
    char request_id[40];
    bool failed;
    int64_t started;
} image_transfer_t;
static image_transfer_t s_image;

extern const char wifi_portal_start[] asm("_binary_wifi_portal_html_start");

static uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t size)
{
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1));
        }
    }
    return ~crc;
}

static void random_hex(char *out, size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i) {
        uint8_t value = (uint8_t)esp_random();
        snprintf(out + i * 2, 3, "%02x", value);
    }
}

static void load_string(nvs_handle_t nvs, const char *key, char *out, size_t capacity)
{
    size_t size = capacity;
    if (nvs_get_str(nvs, key, out, &size) != ESP_OK) out[0] = '\0';
}

static esp_err_t load_settings(void)
{
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), TAG, "Read MAC");
    snprintf(s_device_id, sizeof(s_device_id), "card-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_ap_name, sizeof(s_ap_name), "Card-%02X%02X%02X", mac[3], mac[4], mac[5]);
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("card_net", NVS_READWRITE, &nvs), TAG, "Open network settings");
    load_string(nvs, "ssid", s_ssid, sizeof(s_ssid));
    load_string(nvs, "password", s_password, sizeof(s_password));
    load_string(nvs, "server", s_server, sizeof(s_server));
    load_string(nvs, "secret", s_secret, sizeof(s_secret));
    load_string(nvs, "ap_password", s_ap_password, sizeof(s_ap_password));
    nvs_close(nvs);
    return ESP_OK;
}

static esp_err_t ensure_credentials(void)
{
    /* Wi-Fi is running here, providing entropy for esp_random(). */
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("card_net", NVS_READWRITE, &nvs), TAG, "Open device identity");
    esp_err_t err = ESP_OK;
    if (strlen(s_secret) != 64) {
        random_hex(s_secret, 32);
        err = nvs_set_str(nvs, "secret", s_secret);
    }
    if (err == ESP_OK && strlen(s_ap_password) != 16) {
        random_hex(s_ap_password, 8);
        err = nvs_set_str(nvs, "ap_password", s_ap_password);
    }
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void setup_screen(lv_obj_t *screen)
{
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101C16), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Lineless / Wi-Fi setup");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xB7F5B3), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);
    char text[400];
    snprintf(text, sizeof(text), "Hotspot: %s\nPassword: %s\n\nhttp://192.168.4.1\n\nID: %s\nClaim: %s\n\n%s",
             s_ap_name, s_ap_password, s_device_id,
             s_claimed ? "Assigned" : (s_claim_code[0] ? s_claim_code : "Waiting for network"), s_message);
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, 220);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xEDF6EF), 0);
    lv_obj_set_style_text_line_space(label, 4, 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 10, 48);
}

static void show_setup(void)
{
    if (s_ap_active && card_storage_content_lock(1000)) {
        card_display_show_builder(setup_screen);
        card_storage_content_unlock();
    }
}

static bool portal_request(httpd_req_t *req)
{
    /* Expose the provisioning interface only through the password-protected AP,
       never through the already-connected router interface. */
    struct sockaddr_in local;
    socklen_t size = sizeof(local);
    int fd = httpd_req_to_sockfd(req);
    if (!s_ap_active || getsockname(fd, (struct sockaddr *)&local, &size) != 0 ||
        local.sin_addr.s_addr != inet_addr("192.168.4.1")) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Use the Card setup hotspot");
        return false;
    }
    return true;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}

static esp_err_t portal_root(httpd_req_t *req)
{
    if (!portal_request(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, wifi_portal_start);
}

static esp_err_t portal_status(httpd_req_t *req)
{
    if (!portal_request(req)) return ESP_OK;
    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(json, "device_id", s_device_id);
    cJSON_AddStringToObject(json, "claim_code", s_claim_code);
    cJSON_AddStringToObject(json, "message", s_message);
    cJSON_AddStringToObject(json, "ssid", s_ssid);
    cJSON_AddStringToObject(json, "server", s_server);
    cJSON_AddBoolToObject(json, "claimed", s_claimed);
    cJSON_AddBoolToObject(json, "connected", (xEventGroupGetBits(s_events) & WIFI_READY) != 0);
    return send_json(req, json);
}

static esp_err_t portal_scan(httpd_req_t *req)
{
    if (!portal_request(req)) return ESP_OK;
    cJSON *json = cJSON_CreateObject();
    cJSON *networks = cJSON_AddArrayToObject(json, "networks");
    wifi_scan_config_t config = {.show_hidden = false};
    if (esp_wifi_scan_start(&config, true) == ESP_OK) {
        wifi_ap_record_t records[16];
        uint16_t count = 16;
        if (esp_wifi_scan_get_ap_records(&count, records) == ESP_OK) {
            for (unsigned i = 0; i < count; ++i) {
                cJSON *entry = cJSON_CreateObject();
                cJSON_AddStringToObject(entry, "ssid", (const char *)records[i].ssid);
                cJSON_AddNumberToObject(entry, "rssi", records[i].rssi);
                cJSON_AddItemToArray(networks, entry);
            }
        }
    }
    return send_json(req, json);
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static bool valid_server(const char *url)
{
    if (!url || strlen(url) > 180) return false;
    const char *host = NULL;
    if (strncmp(url, "http://", 7) == 0) host = url + 7;
    if (strncmp(url, "https://", 8) == 0) host = url + 8;
    if (!host || !*host || strstr(host, "localhost") || strncmp(host, "127.", 4) == 0) return false;
    for (const char *p = host; *p; ++p) {
        if ((unsigned char)*p <= 0x20 || *p == '@' || *p == '/' || *p == '?' || *p == '#' || *p == '\\') return false;
    }
    return true;
}

static esp_err_t portal_config(httpd_req_t *req)
{
    if (!portal_request(req)) return ESP_OK;
    char header[80];
    if (httpd_req_get_hdr_value_str(req, "X-Card-Setup", header, sizeof(header)) != ESP_OK || strcmp(header, "1") != 0)
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Missing setup header");
    if (httpd_req_get_hdr_value_len(req, "Origin") > 0 &&
        (httpd_req_get_hdr_value_str(req, "Origin", header, sizeof(header)) != ESP_OK || strcmp(header, "http://192.168.4.1") != 0))
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid setup origin");
    if (req->content_len <= 0 || req->content_len > 768)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Configuration too large");
    char body[769];
    int used = 0;
    while (used < req->content_len) {
        int received = httpd_req_recv(req, body + used, req->content_len - used);
        if (received <= 0) return ESP_FAIL;
        used += received;
    }
    body[used] = '\0';
    cJSON *json = cJSON_Parse(body);
    cJSON *ssid = cJSON_GetObjectItem(json, "ssid");
    cJSON *password = cJSON_GetObjectItem(json, "password");
    cJSON *server = cJSON_GetObjectItem(json, "server");
    if (!cJSON_IsString(ssid) || !cJSON_IsString(password) || !cJSON_IsString(server) ||
        strlen(ssid->valuestring) < 1 || strlen(ssid->valuestring) > 32 ||
        strlen(password->valuestring) > 63 ||
        (strlen(password->valuestring) > 0 && strlen(password->valuestring) < 8) || !valid_server(server->valuestring)) {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid SSID, password, or server URL");
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("card_net", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "ssid", ssid->valuestring);
        if (err == ESP_OK) err = nvs_set_str(nvs, "password", password->valuestring);
        if (err == ESP_OK) err = nvs_set_str(nvs, "server", server->valuestring);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    cJSON_Delete(json);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save configuration");
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "message", "配置已保存。设备即将重启并连接 Wi-Fi；稍后重新连接 Card 热点查看认领码。");
    err = send_json(req, result);
    if (xTaskCreate(restart_task, "card_restart", 2048, NULL, 3, NULL) != pdPASS) return ESP_FAIL;
    return err;
}

static esp_err_t start_portal(void)
{
    if (s_ap_active) return ESP_OK;
    wifi_config_t config = {0};
    strlcpy((char *)config.ap.ssid, s_ap_name, sizeof(config.ap.ssid));
    strlcpy((char *)config.ap.password, s_ap_password, sizeof(config.ap.password));
    config.ap.ssid_len = strlen(s_ap_name);
    config.ap.channel = 1;
    config.ap.max_connection = 4;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "Enable setup AP");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &config), TAG, "Configure setup AP");
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.stack_size = 8192;
    http.max_uri_handlers = 8;
    http.lru_purge_enable = true;
    http.uri_match_fn = httpd_uri_match_wildcard;
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &http), TAG, "Start setup HTTP server");
    s_ap_active = true;
    const httpd_uri_t routes[] = {
        {.uri = "/api/status", .method = HTTP_GET, .handler = portal_status},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = portal_scan},
        {.uri = "/api/config", .method = HTTP_POST, .handler = portal_config},
        {.uri = "/*", .method = HTTP_GET, .handler = portal_root},
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &routes[i]), TAG, "Register setup route");
    }
    ESP_LOGI(TAG, "Setup AP %s ready at http://192.168.4.1", s_ap_name);
    show_setup();
    return ESP_OK;
}

static void restore_content(void)
{
    if (!card_storage_content_lock(5000)) return;
    if (card_storage_file_size(CARD_IMAGE_PATH) == CARD_RGB565_BYTES)
        card_display_show_rgb565_file(CARD_IMAGE_PATH);
    else
        card_display_show_builder(s_fallback);
    if (wasm_plugin_is_loaded()) wasm_plugin_render();
    card_storage_content_unlock();
}

static void stop_portal(void)
{
    if (!s_ap_active) return;
    s_ap_active = false;
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    esp_wifi_set_mode(WIFI_MODE_STA);
    restore_content();
}

static void network_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && s_ssid[0]) esp_wifi_connect();
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, WIFI_READY);
        if (s_ssid[0]) esp_wifi_connect();
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(s_events, WIFI_READY);
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_DATA) {
        http_buffer_t *buffer = event->user_data;
        if (event->data_len < 0 || buffer->used + (size_t)event->data_len >= sizeof(buffer->data)) {
            buffer->overflow = true;
        } else {
            memcpy(buffer->data + buffer->used, event->data, event->data_len);
            buffer->used += event->data_len;
            buffer->data[buffer->used] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t register_device(void)
{
    char url[224];
    snprintf(url, sizeof(url), "%s/api/device/register", s_server);
    http_buffer_t response = {0};
    const esp_http_client_config_t config = {
        .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 10000,
        .event_handler = http_event, .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t http = esp_http_client_init(&config);
    if (!http) return ESP_ERR_NO_MEM;
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "device_id", s_device_id);
    cJSON_AddStringToObject(request, "secret", s_secret);
    cJSON_AddStringToObject(request, "model", "ESP32-S3");
    char *body = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (!body) { esp_http_client_cleanup(http); return ESP_ERR_NO_MEM; }
    esp_http_client_set_header(http, "Content-Type", "application/json");
    esp_http_client_set_post_field(http, body, strlen(body));
    esp_err_t err = esp_http_client_perform(http);
    const int status = esp_http_client_get_status_code(http);
    esp_http_client_cleanup(http);
    free(body);
    if (err != ESP_OK || status != 200 || response.overflow) return ESP_FAIL;
    cJSON *json = cJSON_Parse(response.data);
    cJSON *uri = cJSON_GetObjectItem(json, "mqtt_uri");
    cJSON *code = cJSON_GetObjectItem(json, "claim_code");
    if (!cJSON_IsString(uri) || strlen(uri->valuestring) >= sizeof(s_mqtt_uri) ||
        (strncmp(uri->valuestring, "mqtt://", 7) != 0 && strncmp(uri->valuestring, "mqtts://", 8) != 0)) {
        cJSON_Delete(json);
        return ESP_ERR_INVALID_RESPONSE;
    }
    strlcpy(s_mqtt_uri, uri->valuestring, sizeof(s_mqtt_uri));
    if (cJSON_IsString(code)) strlcpy(s_claim_code, code->valuestring, sizeof(s_claim_code));
    s_claimed = cJSON_IsTrue(cJSON_GetObjectItem(json, "claimed"));
    cJSON_Delete(json);
    s_registered = true;
    strlcpy(s_message, s_claimed ? "Connected. Waiting for assignment." : "Open website and claim this device.", sizeof(s_message));
    if (!s_claimed) start_portal();
    show_setup();
    return ESP_OK;
}

static void publish_status(void)
{
    if (!s_mqtt) return;
    char topic[64], text[192], ip[20] = "0.0.0.0";
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_sta_netif, &info) == ESP_OK) snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    wifi_ap_record_t ap = {0};
    esp_wifi_sta_get_ap_info(&ap);
    snprintf(topic, sizeof(topic), "cards/%s/status", s_device_id);
    snprintf(text, sizeof(text), "{\"state\":\"online\",\"firmware\":\"lineless-v1\",\"ip\":\"%s\",\"rssi\":%d}", ip, ap.rssi);
    esp_mqtt_client_enqueue(s_mqtt, topic, text, 0, 1, 0, true);
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t event = event_data;
    static char payload[768];
    static int used;
    if (id == MQTT_EVENT_CONNECTED) {
        char topic[64];
        snprintf(topic, sizeof(topic), "cards/%s/assignment", s_device_id);
        esp_mqtt_client_subscribe(s_mqtt, topic, 1);
        publish_status();
    } else if (id == MQTT_EVENT_DATA) {
        if (event->current_data_offset == 0) used = 0;
        if (event->total_data_len <= 0 || event->total_data_len >= sizeof(payload) ||
            event->current_data_offset != used || event->data_len < 0 || used + event->data_len >= sizeof(payload)) return;
        memcpy(payload + used, event->data, event->data_len);
        used += event->data_len;
        payload[used] = '\0';
        if (used != event->total_data_len) return;
        cJSON *json = cJSON_Parse(payload);
        cJSON *device = cJSON_GetObjectItem(json, "device_id");
        cJSON *tenant = cJSON_GetObjectItem(json, "tenant_id");
        cJSON *uri = cJSON_GetObjectItem(json, "websocket_url");
        if (cJSON_IsString(device) && strcmp(device->valuestring, s_device_id) == 0 &&
            cJSON_IsString(tenant) && strlen(tenant->valuestring) > 0 && strlen(tenant->valuestring) < 40 &&
            cJSON_IsString(uri) && strlen(uri->valuestring) < 256 &&
            (strncmp(uri->valuestring, "ws://", 5) == 0 || strncmp(uri->valuestring, "wss://", 6) == 0)) {
            assignment_t assignment = {0};
            strlcpy(assignment.tenant, tenant->valuestring, sizeof(assignment.tenant));
            strlcpy(assignment.websocket_url, uri->valuestring, sizeof(assignment.websocket_url));
            xQueueOverwrite(s_assignments, &assignment);
        }
        cJSON_Delete(json);
    }
}

static esp_err_t start_mqtt(void)
{
    esp_mqtt_client_config_t config = {
        .broker.address.uri = s_mqtt_uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.client_id = s_device_id,
        .credentials.username = s_device_id,
        .credentials.authentication.password = s_secret,
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 5000,
        .buffer.size = 1024,
    };
    s_mqtt = esp_mqtt_client_init(&config);
    if (!s_mqtt) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    if (err == ESP_OK) err = esp_mqtt_client_start(s_mqtt);
    if (err != ESP_OK) { esp_mqtt_client_destroy(s_mqtt); s_mqtt = NULL; }
    return err;
}

static void reset_transfer(void)
{
    if (s_image.file) fclose(s_image.file);
    remove(IMAGE_TEMP);
    memset(&s_image, 0, sizeof(s_image));
}

static void acknowledge(bool ok)
{
    char text[180];
    snprintf(text, sizeof(text), "{\"type\":\"image_ack\",\"request_id\":\"%s\",\"ok\":%s,\"crc32\":%lu}",
             s_image.request_id, ok ? "true" : "false", (unsigned long)s_image.crc);
    esp_websocket_client_send_text(s_ws, text, strlen(text), pdMS_TO_TICKS(5000));
}

static void image_control(const char *text)
{
    cJSON *json = cJSON_Parse(text);
    cJSON *type = cJSON_GetObjectItem(json, "type");
    cJSON *request = cJSON_GetObjectItem(json, "request_id");
    if (!cJSON_IsString(type)) { cJSON_Delete(json); return; }
    if (strcmp(type->valuestring, "image_begin") == 0) {
        reset_transfer();
        cJSON *size = cJSON_GetObjectItem(json, "size");
        cJSON *crc = cJSON_GetObjectItem(json, "crc32");
        if (cJSON_IsString(request) && strlen(request->valuestring) == 16 &&
            strspn(request->valuestring, "0123456789abcdef") == 16 &&
            cJSON_IsNumber(size) && size->valuedouble == CARD_RGB565_BYTES &&
            cJSON_IsNumber(crc) && crc->valuedouble >= 0 && crc->valuedouble <= UINT32_MAX) {
            strlcpy(s_image.request_id, request->valuestring, sizeof(s_image.request_id));
            s_image.expected_crc = (uint32_t)crc->valuedouble;
            s_image.started = esp_timer_get_time();
            s_image.file = fopen(IMAGE_TEMP, "wb");
            s_image.failed = s_image.file == NULL;
        }
    } else if (strcmp(type->valuestring, "image_commit") == 0 &&
               cJSON_IsString(request) && s_image.request_id[0] && strcmp(request->valuestring, s_image.request_id) == 0) {
        bool ok = !s_image.failed && s_image.file && s_image.bytes == CARD_RGB565_BYTES &&
                  s_image.crc == s_image.expected_crc && esp_timer_get_time() - s_image.started < 60000000;
        if (s_image.file) {
            if (fflush(s_image.file) != 0) ok = false;
            if (fclose(s_image.file) != 0) ok = false;
            s_image.file = NULL;
        }
        if (ok) {
            ok = card_storage_content_lock(5000);
            if (ok) {
                ok = card_storage_replace(IMAGE_TEMP, CARD_IMAGE_PATH) == ESP_OK;
                if (ok) ok = card_display_show_rgb565_file(CARD_IMAGE_PATH) == ESP_OK;
                if (ok && wasm_plugin_is_loaded()) ok = wasm_plugin_render() == ESP_OK;
                card_storage_content_unlock();
            }
        }
        acknowledge(ok);
        ESP_LOGI(TAG, "Wireless image %s (%u bytes)", ok ? "displayed" : "rejected", (unsigned)s_image.bytes);
        reset_transfer();
    }
    cJSON_Delete(json);
}

static void websocket_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *event = event_data;
    static char text[512];
    static int text_used;
    static int opcode;
    if (id == WEBSOCKET_EVENT_CONNECTED) xEventGroupSetBits(s_events, WS_READY);
    if (id == WEBSOCKET_EVENT_DISCONNECTED) {
        xEventGroupClearBits(s_events, WS_READY);
        if (xSemaphoreTake(s_image_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            reset_transfer();
            xSemaphoreGive(s_image_lock);
        }
    }
    if (id != WEBSOCKET_EVENT_DATA || event->data_len < 0) return;
    if (event->op_code == 0x9 || event->op_code == 0xA || event->op_code == 0x8) return;
    if (event->payload_offset == 0 && event->op_code != 0) opcode = event->op_code;
    if (xSemaphoreTake(s_image_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    if (opcode == 1) {
        if (event->payload_offset == 0) text_used = 0;
        if (event->payload_len < sizeof(text) && event->payload_offset == text_used &&
            text_used + event->data_len < sizeof(text)) {
            memcpy(text + text_used, event->data_ptr, event->data_len);
            text_used += event->data_len;
            text[text_used] = '\0';
            if (text_used == event->payload_len) image_control(text);
        }
    } else if (opcode == 2 && s_image.request_id[0]) {
        if (!s_image.file || s_image.bytes + event->data_len > CARD_RGB565_BYTES ||
            esp_timer_get_time() - s_image.started > 60000000) {
            s_image.failed = true;
        } else {
            if (fwrite(event->data_ptr, 1, event->data_len, s_image.file) != event->data_len) s_image.failed = true;
            s_image.crc = crc_update(s_image.crc, (const uint8_t *)event->data_ptr, event->data_len);
            s_image.bytes += event->data_len;
        }
    }
    xSemaphoreGive(s_image_lock);
}

static esp_err_t start_websocket(const assignment_t *assignment)
{
    if (s_ws) return ESP_OK;
    strlcpy(s_ws_uri, assignment->websocket_url, sizeof(s_ws_uri));
    snprintf(s_ws_headers, sizeof(s_ws_headers), "Authorization: Bearer %s\r\nX-Device-ID: %s\r\n", s_secret, s_device_id);
    esp_websocket_client_config_t config = {
        .uri = s_ws_uri, .headers = s_ws_headers,
        .buffer_size = 4096, .task_stack = 8192,
        .reconnect_timeout_ms = 5000, .network_timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    s_ws = esp_websocket_client_init(&config);
    if (!s_ws) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, websocket_event, NULL);
    if (err == ESP_OK) err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) { esp_websocket_client_destroy(s_ws); s_ws = NULL; }
    return err;
}

static void network_task(void *arg)
{
    (void)arg;
    int64_t last_register = -15000000, last_status = 0, disconnected = esp_timer_get_time();
    int64_t button_since = 0;
    bool button_handled = false;
    bool wifi_timeout_portal = false;
    while (true) {
        const int64_t now = esp_timer_get_time();
        EventBits_t bits = xEventGroupGetBits(s_events);
        if (gpio_get_level(GPIO_NUM_0) == 0) {
            if (!button_since) button_since = now;
            if (!button_handled && now - button_since > 5000000) { s_force_setup = true; button_handled = true; }
        } else { button_since = 0; button_handled = false; }
        if (s_force_setup) { s_force_setup = false; s_manual_setup = true; start_portal(); }
        if (!(bits & WIFI_READY)) {
            if (now - disconnected > 60000000) {
                strlcpy(s_message, "Wi-Fi unavailable. Please configure.", sizeof(s_message));
                /* Only a portal opened for this outage should close on Wi-Fi
                   recovery. Keep existing claim/configuration portals open. */
                if (!s_ap_active && start_portal() == ESP_OK) wifi_timeout_portal = true;
            }
        } else {
            disconnected = now;
            /* A retained WebSocket client reconnects itself. Portal recovery
               must not wait for a new client or a server assignment. */
            if (wifi_timeout_portal) {
                if (!s_manual_setup) stop_portal();
                wifi_timeout_portal = false;
            }
            if (s_server[0] && (!s_registered || !s_claimed) && now - last_register > 15000000) {
                last_register = now;
                if (register_device() != ESP_OK) {
                    strlcpy(s_message, "Cannot reach server. Check server URL.", sizeof(s_message));
                    start_portal();
                    show_setup();
                }
            }
            if (s_registered && !s_mqtt) start_mqtt();
            if (s_mqtt && now - last_status > 20000000) { last_status = now; publish_status(); }
            assignment_t assignment;
            if (xQueueReceive(s_assignments, &assignment, 0) == pdTRUE) {
                s_claimed = true;
                s_claim_code[0] = '\0';
                /* Restore the card before allowing the server to send a new image. */
                if (s_ap_active && !s_manual_setup) stop_portal();
                if (!s_ws) start_websocket(&assignment);
            }
        }
        if (xSemaphoreTake(s_image_lock, 0) == pdTRUE) {
            if (s_image.request_id[0] && now - s_image.started > 60000000) reset_transfer();
            xSemaphoreGive(s_image_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t card_network_start(bool force_setup, card_display_builder_t fallback)
{
    s_fallback = fallback;
    s_manual_setup = force_setup;
    s_events = xEventGroupCreate();
    s_assignments = xQueueCreate(1, sizeof(assignment_t));
    s_image_lock = xSemaphoreCreateMutex();
    if (!s_events || !s_assignments || !s_image_lock || !fallback) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(load_settings(), TAG, "Load settings");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Initialize network");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    if (!s_sta_netif || !ap) return ESP_ERR_NO_MEM;
    wifi_init_config_t wifi = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi), TAG, "Initialize Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Set Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, NULL), TAG, "Wi-Fi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event, NULL), TAG, "IP handler");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Station mode");
    if (s_ssid[0]) {
        wifi_config_t station = {0};
        memcpy(station.sta.ssid, s_ssid, strlen(s_ssid));
        memcpy(station.sta.password, s_password, strlen(s_password));
        station.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        station.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &station), TAG, "Station configuration");
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Start Wi-Fi");
    ESP_RETURN_ON_ERROR(ensure_credentials(), TAG, "Initialize device credentials");
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (force_setup || !s_ssid[0] || !s_server[0]) ESP_RETURN_ON_ERROR(start_portal(), TAG, "Start setup portal");
    if (xTaskCreate(network_task, "card_network", 8192, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Wireless card %s started", s_device_id);
    return ESP_OK;
}
