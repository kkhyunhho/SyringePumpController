#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * Phase-B (read-only) wrappers around the sy01b-server /v1 endpoints.
 *
 * All functions are synchronous and intended to run from the
 * dedicated ``pump_task`` FreeRTOS task — never from the LVGL thread.
 * Each builds a JSON request body (or no body for GET), hits the
 * server via ``esp_http_client``, parses the JSON response, and
 * fills the out-struct. Returns ``ESP_OK`` on HTTP 2xx + valid JSON,
 * or an ``esp_err_t`` on transport / parse / status failure.
 *
 * Phase C will add: pump_initialize, pump_valve, pump_aspirate,
 * pump_dispense, pump_move_steps, pump_prime.
 */

typedef struct {
    bool ok_to_initialize;
    float supply_volts;
    char software_version[32];
    char serial_number[32];
    char valve_position[8];
    int plunger_steps;
    char pre_init_error_name[32];
    int pre_init_error_code;
} pump_diag_t;

typedef struct {
    char valve[8];
    int plunger_steps;
    bool busy;
    char error_name[32];
    int error_code;
} pump_status_t;

/** Initialize the HTTP client (reads server URL from config_store). */
esp_err_t pump_client_init(void);

/** GET /v1/diagnose — runs only once at boot in Phase B. */
esp_err_t pump_diagnose(pump_diag_t *out);

/** GET /v1/status — polled by the Status tab every 2 s. */
esp_err_t pump_status(pump_status_t *out);
