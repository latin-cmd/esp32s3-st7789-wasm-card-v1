#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define CARD_PLUGIN_MAX_BYTES (128 * 1024)

/* Called (with the plugin lock held) when a rejected plugin already drew on
   screen and no previously active plugin can redraw it. Must restore the
   default content (saved image or static card) without calling back into
   wasm_plugin_*. */
typedef void (*wasm_plugin_fallback_t)(void);

esp_err_t wasm_plugin_init(void);
void wasm_plugin_set_fallback(wasm_plugin_fallback_t fallback);
esp_err_t wasm_plugin_load_and_run(const char *path);
esp_err_t wasm_plugin_render(void);
bool wasm_plugin_is_loaded(void);
const char *wasm_plugin_last_error(void);
