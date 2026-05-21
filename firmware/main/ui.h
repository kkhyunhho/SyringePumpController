#pragma once

#include "state.h"

/**
 * LVGL UI for Phase B. Creates a 4-tab tabview but only wires the
 * Status tab — Valve / Move / Prime tabs show "Phase C" placeholders
 * with disabled controls.
 *
 * All functions in this header must be called from the LVGL thread
 * (use ``bsp_display_lock()`` / ``bsp_display_unlock()`` or
 * ``lv_async_call()`` from background tasks).
 */

/** Build the entire tabview and top banner. Call once during boot. */
void ui_create(void);

/** Push the current FSM state to the top banner color/text. */
void ui_apply_state(app_state_t state, const char *banner_text);

/** Refresh the Status tab from a snapshot. Idempotent. */
void ui_apply_status(const app_status_t *status);
