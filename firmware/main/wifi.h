#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * Initialize WiFi STA, start the auto-reconnect task, and block until
 * the first IP is acquired (or the initial retry budget is exhausted).
 * Subsequent disconnects are handled in the background — the public
 * surface beyond this is just ``wifi_is_connected()``.
 *
 * On connect loss, the WiFi task posts ``state_set_wifi_lost()`` to
 * the FSM (see state.h); on reconnect, ``state_set_wifi_connected()``.
 *
 * Returns ESP_OK once an IP is acquired, or an error if the initial
 * SSID/PSK are empty (i.e. nothing was provisioned).
 */
esp_err_t wifi_start(void);

bool wifi_is_connected(void);
