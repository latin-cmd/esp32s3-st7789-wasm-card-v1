/* Host I/O doubles for the production functions selected by test_firmware_network.py.
 * No radio, RTOS scheduling, Flash, TLS or LCD hardware is simulated here. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIT0 1U
#define BIT1 2U
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_FAIL 2
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define GPIO_NUM_0 0
#define WIFI_MODE_STA 1
#define WIFI_EVENT 1
#define IP_EVENT 2
#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_DISCONNECTED 2
#define IP_EVENT_STA_GOT_IP 3
#define WEBSOCKET_EVENT_CONNECTED 1
#define WEBSOCKET_EVENT_DISCONNECTED 2
#define WEBSOCKET_EVENT_DATA 3
#define WEBSOCKET_EVENT_ANY 4
#define CARD_STORAGE_BASE_PATH "/data"
#define CARD_IMAGE_PATH "/data/current.rgb"
#define CARD_RGB565_BYTES ((size_t)134400)
#define ESP_LOGI(...) ((void)0)

typedef int esp_err_t;
typedef unsigned EventBits_t;
typedef int esp_event_base_t;
typedef void *EventGroupHandle_t;
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
typedef void esp_netif_t;
typedef void *httpd_handle_t;
typedef void *esp_mqtt_client_handle_t;
typedef void *esp_websocket_client_handle_t;
typedef void (*card_display_builder_t)(void *);
typedef struct {
    int data_len, op_code, payload_offset, payload_len;
    const char *data_ptr;
} esp_websocket_event_data_t;
typedef struct {
    const char *uri, *headers;
    int buffer_size, task_stack, reconnect_timeout_ms, network_timeout_ms;
    void (*crt_bundle_attach)(void);
} esp_websocket_client_config_t;

static int64_t mock_now;
static EventBits_t mock_bits;
static int portal_starts, portal_stops, restores, shown_images, shown_cards, renders;
static int ws_inits, ws_starts, ws_destroys, transfer_removes, image_controls;
static int content_depth, button_level = 1;
static bool saved_image, plugin_loaded, pending_assignment;
static bool fail_portal, fail_ws_start;
static char actions[64];
static size_t action_count;
static jmp_buf task_done;

static void action(char value) { assert(action_count + 1 < sizeof(actions)); actions[action_count++] = value; }
static void fallback(void *screen) { (void)screen; }
static bool card_storage_content_lock(unsigned timeout) { (void)timeout; assert(!content_depth); ++content_depth; return true; }
static void card_storage_content_unlock(void) { assert(content_depth == 1); --content_depth; }
static size_t card_storage_file_size(const char *path) { assert(!strcmp(path, CARD_IMAGE_PATH)); return saved_image ? CARD_RGB565_BYTES : 0; }
static int card_display_show_rgb565_file(const char *path) { assert(content_depth == 1); assert(!strcmp(path, CARD_IMAGE_PATH)); ++shown_images; ++restores; action('I'); return ESP_OK; }
static int card_display_show_builder(card_display_builder_t builder) { assert(content_depth == 1); assert(builder == fallback); ++shown_cards; ++restores; action('C'); return ESP_OK; }
static bool wasm_plugin_is_loaded(void) { return plugin_loaded; }
static int wasm_plugin_render(void) { assert(content_depth == 1); ++renders; action('P'); return ESP_OK; }
static int httpd_stop(httpd_handle_t server) { assert(server); ++portal_stops; action('H'); return ESP_OK; }
static int esp_wifi_set_mode(int mode) { assert(mode == WIFI_MODE_STA); action('S'); return ESP_OK; }
static int esp_wifi_connect(void) { return ESP_OK; }
static EventBits_t xEventGroupGetBits(EventGroupHandle_t group) { (void)group; return mock_bits; }
static void xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) { (void)group; mock_bits |= bits; }
static void xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits) { (void)group; mock_bits &= ~bits; }
static int xSemaphoreTake(SemaphoreHandle_t lock, unsigned timeout) { (void)lock; (void)timeout; return pdTRUE; }
static void xSemaphoreGive(SemaphoreHandle_t lock) { (void)lock; }
static int64_t esp_timer_get_time(void) { return mock_now; }
static int gpio_get_level(int pin) { assert(pin == GPIO_NUM_0); return button_level; }
static void esp_crt_bundle_attach(void) {}
static size_t strlcpy(char *to, const char *from, size_t capacity) { size_t length = strlen(from); if (capacity) { size_t n = length < capacity - 1 ? length : capacity - 1; memcpy(to, from, n); to[n] = 0; } return length; }

static esp_err_t start_portal(void);
static void show_setup(void) {}
static esp_err_t register_device(void) { return ESP_FAIL; }
static esp_err_t start_mqtt(void) { return ESP_FAIL; }
static void publish_status(void) {}
static int xQueueReceive(QueueHandle_t queue, void *value, unsigned timeout);
static void vTaskDelay(unsigned ticks);
static uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t size) { (void)data; (void)size; return crc; }
static void image_control(const char *text) { (void)text; ++image_controls; }
static int test_remove(const char *path) { assert(!strcmp(path, "/data/wireless.rgb.tmp")); ++transfer_removes; return 0; }
static esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *config) {
    assert(config->reconnect_timeout_ms == 5000);
    assert(!strcmp(config->uri, "wss://example.test/ws/device"));
    ++ws_inits; return (void *)2;
}
static int esp_websocket_register_events(esp_websocket_client_handle_t ws, int id,
    void (*handler)(void *, esp_event_base_t, int32_t, void *), void *arg) {
    (void)handler; (void)arg; assert(ws && id == WEBSOCKET_EVENT_ANY); return ESP_OK;
}
static int esp_websocket_client_start(esp_websocket_client_handle_t ws) { assert(ws); ++ws_starts; action('W'); return fail_ws_start ? ESP_FAIL : ESP_OK; }
static void esp_websocket_client_destroy(esp_websocket_client_handle_t ws) { assert(ws); ++ws_destroys; }

#define remove test_remove
#include "card_network_under_test.inc"
#undef remove

static esp_err_t start_portal(void) {
    if (s_ap_active) return ESP_OK;
    ++portal_starts;
    if (fail_portal) return ESP_FAIL;
    s_ap_active = true; s_httpd = (void *)1;
    return ESP_OK;
}
static int xQueueReceive(QueueHandle_t queue, void *value, unsigned timeout) {
    (void)queue; (void)timeout;
    if (!pending_assignment) return 0;
    assignment_t *assignment = value;
    memset(assignment, 0, sizeof(*assignment));
    strcpy(assignment->tenant, "tenant");
    strcpy(assignment->websocket_url, "wss://example.test/ws/device");
    pending_assignment = false;
    return pdTRUE;
}

typedef struct { int64_t time; bool wifi; int button; bool assignment; } step_t;
static const step_t *steps;
static size_t step_count, step_index;
static void apply_step(void) {
    const step_t *step = &steps[step_index];
    mock_now = step->time; button_level = step->button; pending_assignment = step->assignment;
    network_event(NULL, step->wifi ? IP_EVENT : WIFI_EVENT,
                  step->wifi ? IP_EVENT_STA_GOT_IP : WIFI_EVENT_STA_DISCONNECTED, NULL);
}
static void vTaskDelay(unsigned ticks) {
    assert(ticks == 100);
    if (++step_index == step_count) longjmp(task_done, 1);
    apply_step();
}
static void run(const step_t *scenario, size_t count) {
    steps = scenario; step_count = count; step_index = 0; apply_step();
    if (!setjmp(task_done)) network_task(NULL);
    assert(content_depth == 0);
}
#define RUN(...) do { const step_t scenario[] = {__VA_ARGS__}; run(scenario, sizeof(scenario) / sizeof(*scenario)); } while (0)
static void reset(void) {
    mock_now = 1; mock_bits = 0;
    s_ap_active = s_claimed = s_registered = s_manual_setup = s_force_setup = false;
    s_ws = s_mqtt = s_httpd = NULL;
    s_ssid[0] = s_server[0] = s_claim_code[0] = '\0';
    memset(&s_image, 0, sizeof(s_image)); s_fallback = fallback;
    portal_starts = portal_stops = restores = shown_images = shown_cards = renders = 0;
    ws_inits = ws_starts = ws_destroys = transfer_removes = image_controls = content_depth = 0;
    saved_image = plugin_loaded = pending_assignment = fail_portal = fail_ws_start = false;
    action_count = 0; memset(actions, 0, sizeof(actions));
}
static void existing_portal(bool manual) { s_ap_active = true; s_manual_setup = manual; s_httpd = (void *)1; }
static void test_auto_recovery(void) {
    /* No MQTT registration or assignment: exercise recovery with and without
       an existing WS object, persisted image and active WASM plugin. */
    for (int ws = 0; ws < 2; ++ws) for (int image = 0; image < 2; ++image) for (int plugin = 0; plugin < 2; ++plugin) {
        reset(); s_ws = ws ? (void *)2 : NULL; saved_image = image; plugin_loaded = plugin;
        RUN({1, true, 1, false}, {2, false, 1, false}, {60000002, false, 1, false},
            {61000000, true, 1, false}, {62000000, true, 1, false});
        assert(portal_starts == 1 && portal_stops == 1 && !s_ap_active && !s_httpd);
        assert(restores == 1 && shown_images == image && shown_cards == !image && renders == plugin);
        assert(s_ws == (ws ? (void *)2 : NULL) && ws_inits == 0 && ws_destroys == 0);
    }
}
static void test_short_outage(void) {
    reset(); s_ws = (void *)2;
    RUN({1, true, 1, false}, {2, false, 1, false}, {60000001, false, 1, false}, {60000002, true, 1, false});
    assert(!portal_starts && !portal_stops && !restores);
}
static void test_repeated_outages(void) {
    reset();
    RUN({1, true, 1, false}, {60000002, false, 1, false}, {61000000, true, 1, false},
        {121000001, false, 1, false}, {122000000, true, 1, false});
    assert(portal_starts == 2 && portal_stops == 2 && restores == 2);
}
static void test_boot_manual_portal(void) {
    reset(); existing_portal(true); s_ws = (void *)2;
    RUN({1, false, 1, false}, {60000002, false, 1, false}, {61000000, true, 1, true});
    assert(s_ap_active && s_manual_setup && !portal_stops && !restores && !ws_inits);
}
static void test_long_press_promotes_auto_portal(void) {
    reset(); s_ws = (void *)2;
    RUN({1, true, 1, false}, {60000002, false, 1, false}, {61000000, false, 0, false},
        {67000001, true, 0, true}, {68000000, true, 1, false});
    assert(portal_starts == 1 && s_ap_active && s_manual_setup && !portal_stops && !restores);
}
static void test_long_press_while_connected(void) {
    reset();
    RUN({1, true, 0, false}, {6000002, true, 0, true}, {7000000, true, 1, false});
    assert(portal_starts == 1 && s_manual_setup && s_ap_active && !portal_stops);
    assert(ws_starts == 1);
}
static void test_claim_and_configuration_portals(void) {
    reset(); existing_portal(false);
    RUN({1, true, 1, false}, {60000002, false, 1, false}, {61000000, true, 1, false});
    assert(s_ap_active && !portal_stops && !restores);
    for (int ws = 0; ws < 2; ++ws) {
        reset(); existing_portal(false); s_ws = ws ? (void *)2 : NULL;
        RUN({1, true, 1, true});
        assert(s_claimed && !s_ap_active && portal_stops == 1 && restores == 1);
        assert(ws_inits == !ws && ws_starts == !ws);
        if (!ws) assert(strchr(actions, 'C') < strchr(actions, 'W'));
    }
}
static void test_failed_portal_start_retries(void) {
    reset(); fail_portal = true;
    RUN({1, false, 1, false}, {60000002, false, 1, false}, {61000000, false, 1, false}, {62000000, true, 1, false});
    assert(portal_starts == 2 && !s_ap_active && !portal_stops && !restores);
}
static void test_ws_start_failure_retries(void) {
    reset(); existing_portal(false); fail_ws_start = true;
    RUN({1, true, 1, true}, {2, true, 1, true});
    assert(portal_stops == 1 && restores == 1 && ws_starts == 2 && ws_destroys == 2 && !s_ws);
}
static void test_disconnect_discards_only_partial_image(void) {
    reset(); s_ws = (void *)2; mock_bits = WIFI_READY | WS_READY; saved_image = true;
    s_image.file = tmpfile(); assert(s_image.file); s_image.bytes = 10;
    strcpy(s_image.request_id, "0123456789abcdef");
    network_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(!(mock_bits & WIFI_READY) && s_ws);
    websocket_event(NULL, 0, WEBSOCKET_EVENT_DISCONNECTED, NULL);
    assert(!(mock_bits & WS_READY) && !s_image.file && !s_image.request_id[0] && transfer_removes == 1);
    RUN({1, false, 1, false}, {60000002, false, 1, false}, {61000000, true, 1, false});
    assert(shown_images == 1 && saved_image && s_ws == (void *)2 && !ws_inits);
    websocket_event(NULL, 0, WEBSOCKET_EVENT_CONNECTED, NULL);
    assert(mock_bits & WS_READY);
    esp_websocket_event_data_t event = {.data_len = 2, .op_code = 1, .payload_len = 2, .data_ptr = "{}"};
    websocket_event(NULL, 0, WEBSOCKET_EVENT_DATA, &event);
    assert(image_controls == 1); /* Incoming control messages still reach the image handler. */
}

int main(void) {
    test_auto_recovery();
    test_short_outage();
    test_repeated_outages();
    test_boot_manual_portal();
    test_long_press_promotes_auto_portal();
    test_long_press_while_connected();
    test_claim_and_configuration_portals();
    test_failed_portal_start_retries();
    test_ws_start_failure_retries();
    test_disconnect_discards_only_partial_image();
    puts("Firmware host regression: 10 test groups passed (including 8 recovery combinations).");
    return 0;
}
