#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * Global FSM. Phase B reaches NEEDS_INIT (DIAGNOSING succeeded but the
 * user hasn't tapped Initialize yet) and parks there — Phase C wires
 * the Initialize button to advance to READY. WIFI_LOST is an overlay
 * that re-asserts on disconnect from any state.
 */
typedef enum {
    APP_STATE_BOOT = 0,
    APP_STATE_WIFI_CONNECTING,
    APP_STATE_DIAGNOSING,
    APP_STATE_NEEDS_INIT,
    APP_STATE_WIFI_LOST,
    APP_STATE_ERROR_RECOVERABLE,
} app_state_t;

typedef struct {
    bool busy;           /**< pump_task is processing a command */
    char error_msg[128]; /**< last error shown in the top banner */
    bool wifi_connected;
    /* Cached status snapshot (refreshed by status_task / Status tab). */
    char valve[8]; /**< /v1/status valve string ("?", "1", ...) */
    int plunger_steps;
    bool pump_busy;
    char pump_error_name[32];
    int pump_error_code;
    float supply_volts;
    char software_version[32];
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
void state_set_error(const char *msg);
void state_update_status(const char *valve, int plunger_steps, bool pump_busy,
                         const char *error_name, int error_code);
