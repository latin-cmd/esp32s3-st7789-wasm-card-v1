#pragma once

#include "esp_err.h"

/* Starts the CARD/1 protocol on the ESP32-S3 native USB Serial/JTAG port. */
esp_err_t card_serial_start(void);
