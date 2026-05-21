#pragma once

#include <stdbool.h>

#include "pump_client.h"
#include "state.h"

/**
 * LVGL UI for Phase C. Builds the full 4-tab layout (Valve / Move /
 * Prime / Status) and the top banner.
 *
 * Threading: all functions in this header must be called from the
 * LVGL thread. Background tasks reach the UI via ``lv_async_call``;
 * see the dispatch helpers below.
 *
 * UI → pump_task communication is via the ``pump_cmd_t`` queue
 * declared in ``main.c``. UI callbacks build commands and enqueue
 * them; pump_task pops, runs HTTP synchronously, then schedules an
 * LVGL update via ``ui_apply_*`` helpers.
 */

/** Build the tabview + banner. Call once during boot. */
void ui_create(void);

/** Push the current FSM state to the top banner colour/text and
 *  enable/disable motion controls. */
void ui_apply_state(app_state_t state, const char *banner_text,
                    bool requires_reinit);

/** Refresh the Status tab from a snapshot. */
void ui_apply_status(const app_status_t *status);

/** Refresh the Move tab's "Current contained" label and the Valve
 *  tab's active-port highlight from the latest snapshot. */
void ui_apply_motion_snapshot(const app_status_t *status);

/** Show a modal for a pump error. Recoverable: Retry/Dismiss.
 *  Fatal: Re-initialize only. */
void ui_show_error_modal(const pump_error_t *err);

/** Show a transient toast (auto-dismisses after 2 s). Used for
 *  ValveOverloadError auto-retry notice and similar non-blocking
 *  signals. */
void ui_show_toast(const char *msg);

/** Programmatically switch the tabview to the Status tab (index 3).
 *  Called from the BSP-button handler in main.c via lv_async_call. */
void ui_jump_to_status_tab(void);
