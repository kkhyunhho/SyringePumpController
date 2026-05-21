#pragma once

#include <stdbool.h>

/**
 * Cross-module interface between the LVGL thread and the
 * synchronous-HTTP ``pump_task`` FreeRTOS task. UI callbacks build a
 * ``pump_cmd_t`` and call ``pump_task_enqueue``; the task pops one
 * command at a time, runs the matching ``pump_client`` wrapper, and
 * schedules the LVGL update via ``lv_async_call``.
 *
 * Single in-flight: queue depth is intentionally small (4) and the
 * UI greys out motion controls while state == BUSY, matching the
 * server-side ``asyncio.Lock`` contract.
 */

typedef enum {
    PUMP_CMD_NONE = 0,
    PUMP_CMD_INITIALIZE,
    PUMP_CMD_VALVE,
    PUMP_CMD_ASPIRATE,
    PUMP_CMD_DISPENSE,
    PUMP_CMD_MOVE_STEPS,
    PUMP_CMD_PRIME,
    /** Replays the last enqueued non-retry command (for "Retry"
     *  modals on recoverable errors). */
    PUMP_CMD_RETRY_LAST,
} pump_cmd_kind_t;

typedef struct {
    pump_cmd_kind_t kind;
    union {
        struct {
            int force;
            bool ccw;
        } init;
        struct {
            int port;
            bool ccw;
        } valve;
        struct {
            float target_uL;
        } volume; /**< aspirate + dispense */
        struct {
            int steps;
        } move_steps;
        struct {
            int cycles;
            int source_port;
            int sink_port;
        } prime;
    } payload;
} pump_cmd_t;

/** Spawn the FreeRTOS task and queue. Idempotent. */
void pump_task_start(void);

/**
 * Enqueue a command. Safe to call from any task (including LVGL).
 * Returns true on success, false if the queue is full (UI should
 * show a toast in that case). The function is non-blocking — drops
 * the command rather than backing up the LVGL thread.
 */
bool pump_task_enqueue(const pump_cmd_t *cmd);
