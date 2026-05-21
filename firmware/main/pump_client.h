#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * HTTP wrappers around the sy01b-server /v1 endpoints. All functions
 * are synchronous and intended to run from the dedicated ``pump_task``
 * FreeRTOS task — never from the LVGL thread. Returns ``ESP_OK`` on
 * HTTP 2xx + valid JSON. On HTTP 4xx/5xx the server returns a JSON
 * error envelope; the wrapper parses it into ``pump_error_t`` (if
 * ``err`` is non-NULL) and returns ``ESP_FAIL``. On transport-layer
 * failures (DNS, refused, parse) it returns the underlying
 * ``esp_err_t``.
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

typedef struct {
    char valve[8];
    int plunger_steps;
} pump_motion_result_t;

typedef struct {
    int cycles_done;
    int ul_per_stroke;
    char final_valve[8];
    int final_plunger;
} pump_prime_result_t;

/** Parsed body of an HTTP 4xx/5xx response from the server. */
typedef enum {
    PUMP_ERROR_NONE = 0,
    PUMP_ERROR_RECOVERABLE,
    PUMP_ERROR_FATAL,
} pump_error_class_t;

typedef struct {
    int http_status;
    pump_error_class_t klass;
    char error_name[40]; /**< e.g. "PlungerOverloadError" */
    int code;            /**< driver-side error code, -1 if absent */
    char command[32];    /**< wire command that triggered it */
    char message[160];
} pump_error_t;

/** Initialize the HTTP client (reads server URL from config_store). */
esp_err_t pump_client_init(void);

/** GET /v1/diagnose — runs once at boot. */
esp_err_t pump_diagnose(pump_diag_t *out);

/** GET /v1/status — polled by the Status tab. */
esp_err_t pump_status(pump_status_t *out);

/**
 * POST /v1/initialize {force, ccw}. On success the server returns
 * {valve, plunger_steps}; we fill ``out``. On 4xx/5xx, ``err`` (if
 * non-NULL) is populated and the function returns ``ESP_FAIL``.
 */
esp_err_t pump_initialize(int force, bool ccw, pump_motion_result_t *out,
                          pump_error_t *err);

/** POST /v1/valve {port, ccw}. */
esp_err_t pump_valve(int port, bool ccw, pump_motion_result_t *out,
                     pump_error_t *err);

/** POST /v1/aspirate {target_uL}. */
esp_err_t pump_aspirate(float target_uL, pump_motion_result_t *out,
                        pump_error_t *err);

/** POST /v1/dispense {target_uL}. */
esp_err_t pump_dispense(float target_uL, pump_motion_result_t *out,
                        pump_error_t *err);

/** POST /v1/move_steps {steps}. */
esp_err_t pump_move_steps(int steps, pump_motion_result_t *out,
                          pump_error_t *err);

/** POST /v1/prime {cycles, source_port, sink_port}. */
esp_err_t pump_prime(int cycles, int source_port, int sink_port,
                     pump_prime_result_t *out, pump_error_t *err);
