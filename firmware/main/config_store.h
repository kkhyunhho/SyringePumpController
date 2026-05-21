#pragma once

#include <stddef.h>

#include "esp_err.h"

/**
 * NVS-backed runtime overrides for Kconfig defaults. Currently
 * read-only — the write path is reserved for a future provisioning
 * gesture (held button at boot, captive portal, BLE) and is not wired
 * in Phase B.
 *
 * All three getters fall back to the matching CONFIG_PUMP_* default
 * if the NVS namespace "cfg" lacks the key. They always null-
 * terminate the output buffer and return ESP_OK on success or
 * ESP_ERR_INVALID_SIZE if ``out_len`` is too small for the default.
 */

esp_err_t config_store_init(void);

esp_err_t config_store_get_server_url(char *out, size_t out_len);
esp_err_t config_store_get_wifi_ssid(char *out, size_t out_len);
esp_err_t config_store_get_wifi_password(char *out, size_t out_len);
