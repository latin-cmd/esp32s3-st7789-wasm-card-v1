#include "wasm_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "card_display.h"
#include "card_storage.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "wasm_export.h"

#if CONFIG_WAMR_INTERP_LOADER_MINI
#error "The WAMR mini loader skips module validation; uploaded plugins need CONFIG_WAMR_INTERP_LOADER_NORMAL"
#endif

#define WASM_STACK_BYTES (16 * 1024)
#define WASM_HEAP_BYTES (64 * 1024)
#define WASM_INSTRUCTION_LIMIT (2000000)
/* Linear memory cap in 64 KB pages. It includes the module's own memory and
   the 64 KB app heap WAMR appends; memory.grow can never exceed it. */
#define WASM_MAX_MEMORY_PAGES (16)
#define WASM_THREAD_STACK_BYTES (16 * 1024)
/* Below the LVGL task (priority 4) so a busy plugin never starves display
   refresh. */
#define WASM_THREAD_PRIORITY (3)

/* Per exported-call budgets for host functions. The instruction limit alone
   does not bound run time, because one cheap wasm call can trigger an
   expensive native operation. */
#define HOST_BUDGET_PRESENT (2)
#define HOST_BUDGET_BLIT (4)
#define HOST_BUDGET_PIXELS ((int64_t)CARD_SCREEN_WIDTH * CARD_SCREEN_HEIGHT * 16)

#define FRAME_PIXELS ((size_t)CARD_SCREEN_WIDTH * CARD_SCREEN_HEIGHT)

static const char *TAG = "wasm_plugin";
static SemaphoreHandle_t s_lock;
static bool s_runtime_ready;
static wasm_module_t s_module;
static wasm_module_inst_t s_instance;
static wasm_exec_env_t s_exec_env;
static uint8_t *s_module_bytes;
static uint16_t *s_frame;
static bool s_presented;
static char s_last_error[160] = "No plugin loaded";
static wasm_plugin_fallback_t s_fallback;

static struct {
    int presents;
    int blits;
    int64_t pixels;
} s_budget;

typedef struct {
    wasm_module_inst_t instance;
    wasm_exec_env_t exec_env;
    wasm_function_inst_t function;
    bool succeeded;
} wasm_call_context_t;

static uint16_t frame_color(int32_t color)
{
    uint16_t value = (uint16_t)color;
#if CONFIG_LV_COLOR_16_SWAP
    value = (uint16_t)((value << 8) | (value >> 8));
#endif
    return value;
}

static void frame_fill(uint16_t value)
{
    for (size_t i = 0; i < FRAME_PIXELS; ++i) {
        s_frame[i] = value;
    }
}

static void set_error(const char *message)
{
    strlcpy(s_last_error, message != NULL ? message : "Unknown plugin error", sizeof(s_last_error));
    ESP_LOGE(TAG, "%s", s_last_error);
}

/* Charges one host call against the budget. When the budget is exhausted a
   wasm exception is raised, which aborts the running export as soon as the
   host function returns. */
static bool budget_charge(wasm_exec_env_t exec_env, int *counter, int limit, int64_t pixels,
                          const char *what)
{
    if (counter != NULL) {
        if (*counter >= limit) {
            char message[64];
            snprintf(message, sizeof(message), "host budget exceeded: %s", what);
            wasm_runtime_set_exception(wasm_runtime_get_module_inst(exec_env), message);
            return false;
        }
        ++*counter;
    }
    if (s_budget.pixels + pixels > HOST_BUDGET_PIXELS) {
        wasm_runtime_set_exception(wasm_runtime_get_module_inst(exec_env),
                                   "host budget exceeded: pixels");
        return false;
    }
    s_budget.pixels += pixels;
    return true;
}

static void host_clear(wasm_exec_env_t exec_env, int32_t color)
{
    if (s_frame == NULL || !budget_charge(exec_env, NULL, 0, (int64_t)FRAME_PIXELS, "clear")) {
        return;
    }
    frame_fill(frame_color(color));
}

static void host_fill_rect(wasm_exec_env_t exec_env, int32_t x, int32_t y,
                           int32_t width, int32_t height, int32_t color)
{
    if (s_frame == NULL || width <= 0 || height <= 0) {
        return;
    }
    /* 64-bit arithmetic so x + width cannot overflow for any int32 input. */
    const int64_t x0 = x < 0 ? 0 : x;
    const int64_t y0 = y < 0 ? 0 : y;
    const int64_t x_end = (int64_t)x + width;
    const int64_t y_end = (int64_t)y + height;
    const int64_t x1 = x_end > CARD_SCREEN_WIDTH ? CARD_SCREEN_WIDTH : x_end;
    const int64_t y1 = y_end > CARD_SCREEN_HEIGHT ? CARD_SCREEN_HEIGHT : y_end;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    if (!budget_charge(exec_env, NULL, 0, (x1 - x0) * (y1 - y0), "fill_rect")) {
        return;
    }
    const uint16_t value = frame_color(color);
    for (int64_t row = y0; row < y1; ++row) {
        uint16_t *dst = s_frame + row * CARD_SCREEN_WIDTH + x0;
        for (int64_t col = x0; col < x1; ++col) {
            *dst++ = value;
        }
    }
}

static int32_t host_blit_image(wasm_exec_env_t exec_env, int32_t image_id,
                               int32_t x, int32_t y)
{
    if (s_frame == NULL || image_id != 0) {
        return -1;
    }
    /* Offsets that leave nothing visible are rejected before any arithmetic,
       so the column/row computations below cannot overflow. */
    if (x <= -CARD_SCREEN_WIDTH || x >= CARD_SCREEN_WIDTH ||
        y <= -CARD_SCREEN_HEIGHT || y >= CARD_SCREEN_HEIGHT) {
        return 0;
    }
    if (!budget_charge(exec_env, &s_budget.blits, HOST_BUDGET_BLIT, (int64_t)FRAME_PIXELS,
                       "blit_image")) {
        return -4;
    }
    FILE *file = fopen(CARD_IMAGE_PATH, "rb");
    if (file == NULL) {
        return -2;
    }

    const int32_t src_x = x < 0 ? -x : 0;
    const int32_t dst_x = x < 0 ? 0 : x;
    const int32_t count = CARD_SCREEN_WIDTH - (x < 0 ? -x : x); /* 1..240 */

    uint16_t row[CARD_SCREEN_WIDTH];
    for (int32_t src_y = 0; src_y < CARD_SCREEN_HEIGHT; ++src_y) {
        if (fread(row, sizeof(row), 1, file) != 1) {
            fclose(file);
            return -3;
        }
        const int32_t dst_y = y + src_y;
        if (dst_y < 0 || dst_y >= CARD_SCREEN_HEIGHT) {
            continue;
        }
        memcpy(s_frame + dst_y * CARD_SCREEN_WIDTH + dst_x,
               row + src_x, (size_t)count * sizeof(uint16_t));
    }
    fclose(file);
    return 0;
}

static int32_t host_present(wasm_exec_env_t exec_env)
{
    if (s_frame == NULL) {
        return -1;
    }
    if (!budget_charge(exec_env, &s_budget.presents, HOST_BUDGET_PRESENT, 0, "present")) {
        return -2;
    }
    const esp_err_t err = card_display_show_rgb565_copy(s_frame, CARD_RGB565_BYTES);
    if (err == ESP_OK) {
        s_presented = true;
    }
    return err == ESP_OK ? 0 : -(int32_t)err;
}

static NativeSymbol s_native_symbols[] = {
    {"host_clear", host_clear, "(i)", NULL},
    {"host_fill_rect", host_fill_rect, "(iiiii)", NULL},
    {"host_blit_image", host_blit_image, "(iii)i", NULL},
    {"host_present", host_present, "()i", NULL},
};

static void destroy_plugin(wasm_module_t module, wasm_module_inst_t instance,
                           wasm_exec_env_t exec_env, uint8_t *bytes)
{
    if (exec_env != NULL) {
        wasm_runtime_destroy_exec_env(exec_env);
    }
    if (instance != NULL) {
        wasm_runtime_deinstantiate(instance);
    }
    if (module != NULL) {
        wasm_runtime_unload(module);
    }
    free(bytes);
}

static void *wasm_call_thread(void *argument)
{
    wasm_call_context_t *context = argument;
    if (!wasm_runtime_init_thread_env()) {
        context->succeeded = false;
        return NULL;
    }
    /* wasm_runtime_call_wasm() re-binds the exec env's thread handle and
       native stack boundary to the calling thread on every call (WAMR 2.4,
       wasm_call_function -> wasm_exec_env_set_thread_info), so using an exec
       env created on another task is safe. */
    wasm_runtime_clear_exception(context->instance);
    wasm_runtime_set_instruction_count_limit(context->exec_env, WASM_INSTRUCTION_LIMIT);
    context->succeeded = wasm_runtime_call_wasm(context->exec_env, context->function, 0, NULL);
    wasm_runtime_destroy_thread_env();
    return NULL;
}

static esp_err_t call_export(wasm_module_inst_t instance, wasm_exec_env_t exec_env,
                             const char *name, bool required)
{
    wasm_function_inst_t function = wasm_runtime_lookup_function(instance, name);
    if (function == NULL) {
        if (required) {
            snprintf(s_last_error, sizeof(s_last_error), "Missing export: %s", name);
            return ESP_ERR_NOT_FOUND;
        }
        return ESP_OK;
    }

    wasm_call_context_t context = {
        .instance = instance,
        .exec_env = exec_env,
        .function = function,
        .succeeded = false,
    };

    /* The esp_pthread config applies to threads created by the calling task;
       it is how the worker gets a priority below the LVGL task. */
    esp_pthread_cfg_t thread_cfg = esp_pthread_get_default_config();
    thread_cfg.stack_size = WASM_THREAD_STACK_BYTES;
    thread_cfg.prio = WASM_THREAD_PRIORITY;
    thread_cfg.thread_name = "wasm_call";
    esp_pthread_set_cfg(&thread_cfg);

    pthread_attr_t attributes;
    pthread_t thread;
    if (pthread_attr_init(&attributes) != 0) {
        snprintf(s_last_error, sizeof(s_last_error), "Unable to configure %s", name);
        return ESP_FAIL;
    }
    memset(&s_budget, 0, sizeof(s_budget));
    if (pthread_attr_setstacksize(&attributes, WASM_THREAD_STACK_BYTES) != 0 ||
        pthread_create(&thread, &attributes, wasm_call_thread, &context) != 0) {
        pthread_attr_destroy(&attributes);
        snprintf(s_last_error, sizeof(s_last_error), "Unable to start %s", name);
        return ESP_FAIL;
    }
    pthread_attr_destroy(&attributes);
    if (pthread_join(thread, NULL) != 0) {
        snprintf(s_last_error, sizeof(s_last_error), "Unable to join %s", name);
        return ESP_FAIL;
    }
    if (!context.succeeded) {
        const char *exception = wasm_runtime_get_exception(instance);
        snprintf(s_last_error, sizeof(s_last_error), "%s failed: %s", name,
                 exception != NULL ? exception : "unknown trap");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Runs plugin_render on the given instance. Caller holds s_lock. */
static esp_err_t render_locked(wasm_module_inst_t instance, wasm_exec_env_t exec_env)
{
    frame_fill(0x0000);
    s_presented = false;
    esp_err_t result = call_export(instance, exec_env, "plugin_render", true);
    if (result == ESP_OK && !s_presented) {
        result = card_display_show_rgb565_copy(s_frame, CARD_RGB565_BYTES);
        s_presented = result == ESP_OK;
    }
    return result;
}

esp_err_t wasm_plugin_init(void)
{
    if (s_runtime_ready) {
        return ESP_OK;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_frame == NULL) {
        s_frame = heap_caps_calloc(FRAME_PIXELS, sizeof(uint16_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_lock == NULL || s_frame == NULL) {
        set_error("Unable to allocate plugin resources in PSRAM");
        return ESP_ERR_NO_MEM;
    }

    RuntimeInitArgs args;
    memset(&args, 0, sizeof(args));
    args.mem_alloc_type = Alloc_With_System_Allocator;
    args.native_module_name = "env";
    args.native_symbols = s_native_symbols;
    args.n_native_symbols = sizeof(s_native_symbols) / sizeof(s_native_symbols[0]);
    args.running_mode = Mode_Interp;
    if (!wasm_runtime_full_init(&args)) {
        set_error("WAMR initialization failed");
        return ESP_FAIL;
    }
    s_runtime_ready = true;
    strlcpy(s_last_error, "Runtime ready", sizeof(s_last_error));
    ESP_LOGI(TAG, "WAMR interpreter ready");
    return ESP_OK;
}

void wasm_plugin_set_fallback(wasm_plugin_fallback_t fallback)
{
    s_fallback = fallback;
}

esp_err_t wasm_plugin_load_and_run(const char *path)
{
    if (!s_runtime_ready || path == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    esp_err_t result = ESP_FAIL;
    bool screen_touched = false;
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        set_error("Plugin file not found");
        result = ESP_ERR_NOT_FOUND;
        goto done;
    }
    fseek(file, 0, SEEK_END);
    const long file_size = ftell(file);
    rewind(file);
    if (file_size < 8 || file_size > CARD_PLUGIN_MAX_BYTES) {
        fclose(file);
        set_error("Plugin size must be between 8 bytes and 128 KB");
        result = ESP_ERR_INVALID_SIZE;
        goto done;
    }

    uint8_t *new_bytes = heap_caps_malloc((size_t)file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (new_bytes == NULL) {
        fclose(file);
        set_error("Out of memory loading plugin");
        result = ESP_ERR_NO_MEM;
        goto done;
    }
    if (fread(new_bytes, (size_t)file_size, 1, file) != 1) {
        fclose(file);
        free(new_bytes);
        set_error("Plugin read failed");
        goto done;
    }
    fclose(file);
    if (memcmp(new_bytes, "\0asm", 4) != 0) {
        free(new_bytes);
        set_error("File is not a WebAssembly module");
        result = ESP_ERR_INVALID_ARG;
        goto done;
    }

    char error[160] = {0};
    wasm_module_t new_module = wasm_runtime_load(new_bytes, (uint32_t)file_size,
                                                  error, sizeof(error));
    if (new_module == NULL) {
        free(new_bytes);
        set_error(error);
        goto done;
    }

    const InstantiationArgs inst_args = {
        .default_stack_size = WASM_STACK_BYTES,
        .host_managed_heap_size = WASM_HEAP_BYTES,
        .max_memory_pages = WASM_MAX_MEMORY_PAGES,
    };
    wasm_module_inst_t new_instance =
        wasm_runtime_instantiate_ex(new_module, &inst_args, error, sizeof(error));
    if (new_instance == NULL) {
        destroy_plugin(new_module, NULL, NULL, new_bytes);
        set_error(error);
        goto done;
    }
    /* max_memory_pages only limits growth; a module whose *initial* memory is
       already above the cap is still instantiated, so reject it here. */
    wasm_memory_inst_t memory = wasm_runtime_get_default_memory(new_instance);
    if (memory != NULL && wasm_memory_get_cur_page_count(memory) > WASM_MAX_MEMORY_PAGES) {
        destroy_plugin(new_module, new_instance, NULL, new_bytes);
        snprintf(s_last_error, sizeof(s_last_error), "Plugin memory exceeds %d KB",
                 WASM_MAX_MEMORY_PAGES * 64);
        ESP_LOGE(TAG, "%s", s_last_error);
        result = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    wasm_exec_env_t new_exec_env = wasm_runtime_create_exec_env(new_instance, WASM_STACK_BYTES);
    if (new_exec_env == NULL) {
        destroy_plugin(new_module, new_instance, NULL, new_bytes);
        set_error("Unable to create plugin execution environment");
        result = ESP_ERR_NO_MEM;
        goto done;
    }

    frame_fill(0x0000);
    s_presented = false;
    result = call_export(new_instance, new_exec_env, "plugin_init", false);
    screen_touched = s_presented;
    if (result == ESP_OK) {
        result = render_locked(new_instance, new_exec_env);
        screen_touched = screen_touched || s_presented;
    }
    if (result != ESP_OK) {
        destroy_plugin(new_module, new_instance, new_exec_env, new_bytes);
        ESP_LOGE(TAG, "%s", s_last_error);
        goto done;
    }

    destroy_plugin(s_module, s_instance, s_exec_env, s_module_bytes);
    s_module = new_module;
    s_instance = new_instance;
    s_exec_env = new_exec_env;
    s_module_bytes = new_bytes;
    strlcpy(s_last_error, "Plugin loaded", sizeof(s_last_error));
    ESP_LOGI(TAG, "Plugin loaded: %ld bytes", file_size);

done:
    if (result != ESP_OK && screen_touched) {
        /* The rejected plugin already drew on screen. Put back what was shown
           before it: the active plugin, or the image / static card fallback. */
        char saved_error[sizeof(s_last_error)];
        strlcpy(saved_error, s_last_error, sizeof(saved_error));
        if (s_instance == NULL || render_locked(s_instance, s_exec_env) != ESP_OK) {
            if (s_fallback != NULL) {
                s_fallback();
            }
        }
        strlcpy(s_last_error, saved_error, sizeof(s_last_error));
    }
    xSemaphoreGive(s_lock);
    return result;
}

esp_err_t wasm_plugin_render(void)
{
    if (!s_runtime_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (s_instance != NULL && s_exec_env != NULL) {
        result = render_locked(s_instance, s_exec_env);
    } else {
        strlcpy(s_last_error, "No plugin loaded", sizeof(s_last_error));
    }
    xSemaphoreGive(s_lock);
    return result;
}

bool wasm_plugin_is_loaded(void)
{
    return s_instance != NULL;
}

const char *wasm_plugin_last_error(void)
{
    return s_last_error;
}
