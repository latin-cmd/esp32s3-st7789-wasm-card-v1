#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "card_display.h"

/* force_setup also preserves the existing BOOT-button plugin recovery mode. */
esp_err_t card_network_start(bool force_setup, card_display_builder_t fallback);
