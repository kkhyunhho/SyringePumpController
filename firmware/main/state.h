#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pump_client.h"

/**
 * Global FSM. Two layers:
 *
 *   - The state itself, ordered loosely by lifecycle:
 *       BOOT → WIFI_CONNECTING → DIAGNOSING → NEEDS_INIT
 *            → READY ⇄ BUSY
 *            → ERROR_RECOVERABLE / ERROR_FATAL
 *            (WIFI_LOST is an overlay set by the wifi task; the
 *            previous state is restored on reconnect.)
 *
 *   - The cached ``app_status_t`` snapshot, updated by both the
 *     status_task (read-only polling) and pump_task (after each
 *     motion command). LVGL reads via ``state_snapshot()``.
 *
 * Phase C adds the BUSY / ERROR_FATAL transitions and the
 * ``requires_reinit`` latch (set by PlungerOverloadError or
 * InitFailedError; cleared by a successful re-init).
 */
typedef enum {
    APP_STATE_BOOT = 0,
    APP_STATE_WIFI_CONNECTING,
    APP_STATE_DIAGNOSING,
    APP_STATE_NEEDS_INIT,
    APP_STATE_READY,
    APP_STATE_BUSY,
    APP_STATE_WIFI_LOST,
    APP_STATE_ERROR_RECOVERABLE,
    APP_STATE_ERROR_FATAL,
} app_state_t;

typedef struct {
    bool busy;           /**< pump_task is processing a command */
    char error_msg[128]; /**< last error shown in the top banner */
    bool wifi_connected;
    bool requires_reinit; /**< set on fatal pump error */
    /* Cached status snapshot (refreshed by status_task / Status tab). */
    char valve[8]; /**< /v1/status valve string ("?", "1", ...) */
    int plunger_steps;
    bool pump_busy;
    char pump_error_name[32];
    int pump_error_code;
    float supply_volts;
    char software_version[32];
    /* Last-error envelope from a motion attempt (populated by
     * state_record_pump_error). Cleared on successful re-init. */
    char last_error_name[40];
    char last_error_message[160];
    int last_error_code;
} app_status_t;

void state_init(void);

app_state_t state_get(void);

/** Subset of the cached status fields; safe to read from LVGL. */
void state_snapshot(app_status_t *out);

/* Event injection (called from wifi/pump tasks, never from LVGL). */
void state_set_wifi_connected(void);
void state_set_wifi_lost(void);
void state_set_diagnosing(void);
void state_set_needs_init(const char *software_version, float supply_volts);
void state_set_ready(void);
void state_set_busy(void);
void state_set_error_recoverable(const char *msg);
void state_set_error_fatal(const char *msg);
/** Legacy alias retained for callers that don't yet distinguish. */
void state_set_error(const char *msg);
void state_update_status(const char *valve, int plunger_steps, bool pump_busy,
                         const char *error_name, int error_code);

/**
 * Record a server-reported motion error. Decides recoverable vs
 * fatal based on ``err->klass``, flips ``requires_reinit`` on
 * fatal, and stashes the envelope so the UI modal can display it.
 */
void state_record_pump_error(const pump_error_t *err);

/** Cleared automatically by a successful ``state_set_ready()``. */
bool state_requires_reinit(void);
