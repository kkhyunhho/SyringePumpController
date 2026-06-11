#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "bsp/esp-box-3.h"
#include "lvgl.h"

#include "config_store.h"
#include "pump_client.h"
#include "pump_task.h"
#include "state.h"
#include "ui.h"
#include "wifi.h"

static const char *TAG = "main";

/* ===================================================== Pump queue & task */

#define PUMP_QUEUE_DEPTH 4

static QueueHandle_t s_pump_queue;
static SemaphoreHandle_t s_last_cmd_mtx;
/* Saved last command so the "Retry" modal can replay it. */
static pump_cmd_t s_last_cmd = {.kind = PUMP_CMD_NONE};

/* ===================================================== UI dispatch helpers
 *
 * All UI mutation (banner, status table, modals) must happen on the
 * LVGL thread. We marshal asynchronously via ``lv_async_call``.
 * Snapshot data is captured at enqueue time so the LVGL handler sees
 * a stable copy.
 */
typedef struct {
    app_state_t state;
    bool requires_reinit;
    char banner[160]; /* must fit app_status_t.error_msg (128) or a
                       * "Fatal: re-init required"-style literal. */
    app_status_t snap;
} ui_state_msg_t;

static void apply_ui_state_async(void *arg)
{
    ui_state_msg_t *m = (ui_state_msg_t *)arg;
    bsp_display_lock(0);
    ui_apply_state(m->state, m->banner, m->requires_reinit);
    ui_apply_status(&m->snap);
    ui_apply_motion_snapshot(&m->snap);
    bsp_display_unlock();
    free(m);
}

static void schedule_ui_refresh(void)
{
    ui_state_msg_t *m = calloc(1, sizeof(*m));
    if (m == NULL) {
        ESP_LOGE(TAG, "ui refresh: oom");
        return;
    }
    m->state = state_get();
    state_snapshot(&m->snap);
    m->requires_reinit = m->snap.requires_reinit;
    switch (m->state) {
    case APP_STATE_BOOT:
        snprintf(m->banner, sizeof(m->banner), "Booting");
        break;
    case APP_STATE_WIFI_CONNECTING:
        snprintf(m->banner, sizeof(m->banner), "Connecting WiFi");
        break;
    case APP_STATE_DIAGNOSING:
        snprintf(m->banner, sizeof(m->banner), "Diagnosing");
        break;
    case APP_STATE_NEEDS_INIT:
        snprintf(m->banner, sizeof(m->banner), "Needs Initialize (press →)");
        break;
    case APP_STATE_READY:
        snprintf(m->banner, sizeof(m->banner), "Ready");
        break;
    case APP_STATE_BUSY:
        snprintf(m->banner, sizeof(m->banner), "Busy");
        break;
    case APP_STATE_WIFI_LOST:
    case APP_STATE_ERROR_RECOVERABLE:
        snprintf(m->banner, sizeof(m->banner), "%s",
                 m->snap.error_msg[0] != '\0' ? m->snap.error_msg : "Error");
        break;
    case APP_STATE_ERROR_FATAL:
        snprintf(m->banner, sizeof(m->banner), "Fatal: re-init required");
        break;
    default:
        snprintf(m->banner, sizeof(m->banner), "Unknown");
        break;
    }
    lv_async_call(apply_ui_state_async, m);
}

static void show_error_modal_async(void *arg)
{
    pump_error_t *err = (pump_error_t *)arg;
    bsp_display_lock(0);
    ui_show_error_modal(err);
    bsp_display_unlock();
    free(err);
}

static void schedule_error_modal(const pump_error_t *err)
{
    /* Transport-level failures (server unreachable, TCP reset, JSON
     * parse failure) have ``http_status == 0`` because no HTTP response
     * was received. Suppress the modal in that case — the banner
     * already turns red and the Status tab's Reconnect button is the
     * recovery path. The modal is only useful for *pump-side* errors
     * the server actually reported, e.g. PlungerOverloadError. */
    if (err->http_status == 0) {
        ESP_LOGW(TAG, "transport-level error (http_status=0), modal "
                      "suppressed: %s",
                 err->error_name[0] != '\0' ? err->error_name : "(unnamed)");
        return;
    }
    pump_error_t *copy = malloc(sizeof(*copy));
    if (copy == NULL) {
        ESP_LOGE(TAG, "error modal: oom");
        return;
    }
    *copy = *err;
    lv_async_call(show_error_modal_async, copy);
}

static void show_toast_async(void *arg)
{
    char *msg = (char *)arg;
    bsp_display_lock(0);
    ui_show_toast(msg);
    bsp_display_unlock();
    free(msg);
}

static void schedule_toast(const char *msg)
{
    char *copy = strdup(msg);
    if (copy == NULL) {
        return;
    }
    lv_async_call(show_toast_async, copy);
}

/* ===================================================== status_task */

static void status_task(void *unused)
{
    (void)unused;
    const TickType_t period =
        pdMS_TO_TICKS(CONFIG_PUMP_STATUS_POLL_INTERVAL_MS);
    while (true) {
        if (wifi_is_connected()) {
            pump_status_t st;
            esp_err_t err = pump_status(&st);
            if (err == ESP_OK) {
                state_update_status(st.valve, st.plunger_steps, st.busy,
                                    st.error_name, st.error_code);
            } else {
                ESP_LOGW(TAG, "status poll failed: %s", esp_err_to_name(err));
            }
            schedule_ui_refresh();
        }
        vTaskDelay(period);
    }
}

/* ===================================================== Diagnose / Init helpers
 */

static esp_err_t run_diagnose_once(void)
{
    pump_diag_t diag;
    state_set_diagnosing();
    schedule_ui_refresh();
    esp_err_t err = pump_diagnose(&diag);
    if (err != ESP_OK) {
        state_set_error_recoverable("diagnose failed — check server");
        schedule_ui_refresh();
        return err;
    }
    if (!diag.ok_to_initialize) {
        ESP_LOGW(TAG, "diagnose says NOT ok_to_initialize");
    }
    state_set_pump_config(diag.syringe_uL, diag.stroke_steps);
    state_set_needs_init(diag.software_version, diag.supply_volts);
    state_update_status(diag.valve_position, diag.plunger_steps, false,
                        diag.pre_init_error_name, diag.pre_init_error_code);
    schedule_ui_refresh();
    return ESP_OK;
}

/* ===================================================== Pump task body */

static void save_last_cmd(const pump_cmd_t *cmd)
{
    if (cmd->kind == PUMP_CMD_RETRY_LAST || cmd->kind == PUMP_CMD_DIAGNOSE) {
        /* Don't overwrite — RETRY_LAST is the retry path itself, and
         * DIAGNOSE isn't a motion command so the Retry-modal shouldn't
         * replay it. */
        return;
    }
    xSemaphoreTake(s_last_cmd_mtx, portMAX_DELAY);
    s_last_cmd = *cmd;
    xSemaphoreGive(s_last_cmd_mtx);
}

static pump_cmd_t fetch_last_cmd(void)
{
    pump_cmd_t cmd;
    xSemaphoreTake(s_last_cmd_mtx, portMAX_DELAY);
    cmd = s_last_cmd;
    xSemaphoreGive(s_last_cmd_mtx);
    return cmd;
}

static void handle_init(const pump_cmd_t *cmd)
{
    pump_motion_result_t res;
    pump_error_t err;
    esp_err_t rc = pump_initialize(cmd->payload.init.force,
                                   cmd->payload.init.ccw, &res, &err);
    if (rc == ESP_OK) {
        state_update_status(res.valve, res.plunger_steps, false, "OK", 0);
        state_set_ready();
    } else {
        state_record_pump_error(&err);
        schedule_error_modal(&err);
    }
}

static void handle_valve(const pump_cmd_t *cmd, bool *retried_overload)
{
    pump_motion_result_t res;
    pump_error_t err;
    esp_err_t rc =
        pump_valve(cmd->payload.valve.port, cmd->payload.valve.ccw, &res, &err);
    if (rc == ESP_OK) {
        state_update_status(res.valve, res.plunger_steps, false, "OK", 0);
        state_set_ready();
        return;
    }
    /* Auto-retry once on ValveOverloadError — server re-homes valve on next
     * cmd per CLAUDE.md "Error model" code 10. */
    if (!*retried_overload && rc == ESP_FAIL &&
        strcmp(err.error_name, "ValveOverloadError") == 0) {
        *retried_overload = true;
        ESP_LOGW(TAG, "valve overload — auto-retry once");
        schedule_toast("Valve overload — retrying");
        handle_valve(cmd, retried_overload);
        return;
    }
    state_record_pump_error(&err);
    schedule_error_modal(&err);
}

static void handle_volume(pump_cmd_kind_t kind, const pump_cmd_t *cmd)
{
    pump_motion_result_t res;
    pump_error_t err;
    esp_err_t rc =
        (kind == PUMP_CMD_ASPIRATE)
            ? pump_aspirate(cmd->payload.volume.target_uL, &res, &err)
            : pump_dispense(cmd->payload.volume.target_uL, &res, &err);
    if (rc == ESP_OK) {
        state_update_status(res.valve, res.plunger_steps, false, "OK", 0);
        state_set_ready();
    } else {
        state_record_pump_error(&err);
        schedule_error_modal(&err);
    }
}

static void handle_move_steps(const pump_cmd_t *cmd)
{
    pump_motion_result_t res;
    pump_error_t err;
    esp_err_t rc = pump_move_steps(cmd->payload.move_steps.steps, &res, &err);
    if (rc == ESP_OK) {
        state_update_status(res.valve, res.plunger_steps, false, "OK", 0);
        state_set_ready();
    } else {
        state_record_pump_error(&err);
        schedule_error_modal(&err);
    }
}

/**
 * Re-run ``GET /v1/diagnose`` on demand (triggered by the Status tab's
 * Reconnect button). On success, ``state_set_needs_init`` followed by
 * ``state_update_status`` lets the existing NEEDS_INIT → READY
 * auto-advance kick in if the pump is already homed; otherwise the
 * banner stays amber until the operator initialises. On failure, the
 * recoverable-error path keeps the Reconnect button reachable for
 * another retry.
 */
static void handle_diagnose(void)
{
    pump_diag_t diag;
    state_set_diagnosing();
    schedule_ui_refresh();
    esp_err_t err = pump_diagnose(&diag);
    if (err != ESP_OK) {
        state_set_error_recoverable("diagnose failed — check server");
        return;
    }
    state_set_pump_config(diag.syringe_uL, diag.stroke_steps);
    state_set_needs_init(diag.software_version, diag.supply_volts);
    state_update_status(diag.valve_position, diag.plunger_steps, false,
                        diag.pre_init_error_name, diag.pre_init_error_code);
}

static void handle_prime(const pump_cmd_t *cmd)
{
    pump_prime_result_t res;
    pump_error_t err;
    esp_err_t rc =
        pump_prime(cmd->payload.prime.cycles, cmd->payload.prime.source_port,
                   cmd->payload.prime.sink_port, cmd->payload.prime.volume_uL,
                   &res, &err);
    if (rc == ESP_OK) {
        state_update_status(res.final_valve, res.final_plunger, false, "OK", 0);
        state_set_ready();
    } else {
        state_record_pump_error(&err);
        schedule_error_modal(&err);
    }
}

static void process_one(pump_cmd_t cmd)
{
    if (cmd.kind == PUMP_CMD_RETRY_LAST) {
        cmd = fetch_last_cmd();
        if (cmd.kind == PUMP_CMD_NONE) {
            ESP_LOGW(TAG, "retry requested but no prior command");
            return;
        }
    } else {
        save_last_cmd(&cmd);
    }

    state_set_busy();
    schedule_ui_refresh();

    bool retried_overload = false;
    switch (cmd.kind) {
    case PUMP_CMD_INITIALIZE:
        handle_init(&cmd);
        break;
    case PUMP_CMD_VALVE:
        handle_valve(&cmd, &retried_overload);
        break;
    case PUMP_CMD_ASPIRATE:
    case PUMP_CMD_DISPENSE:
        handle_volume(cmd.kind, &cmd);
        break;
    case PUMP_CMD_MOVE_STEPS:
        handle_move_steps(&cmd);
        break;
    case PUMP_CMD_PRIME:
        handle_prime(&cmd);
        break;
    case PUMP_CMD_DIAGNOSE:
        handle_diagnose();
        break;
    default:
        ESP_LOGW(TAG, "unknown cmd kind %d", (int)cmd.kind);
        state_set_ready();
        break;
    }
    schedule_ui_refresh();
}

static void pump_task_body(void *unused)
{
    (void)unused;
    pump_cmd_t cmd;
    while (true) {
        if (xQueueReceive(s_pump_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            process_one(cmd);
        }
    }
}

void pump_task_start(void)
{
    if (s_pump_queue != NULL) {
        return;
    }
    s_pump_queue = xQueueCreate(PUMP_QUEUE_DEPTH, sizeof(pump_cmd_t));
    s_last_cmd_mtx = xSemaphoreCreateMutex();
    xTaskCreate(pump_task_body, "pump_task", 8 * 1024, NULL, 5, NULL);
}

bool pump_task_enqueue(const pump_cmd_t *cmd)
{
    if (s_pump_queue == NULL || cmd == NULL) {
        return false;
    }
    return xQueueSend(s_pump_queue, cmd, 0) == pdTRUE;
}

/* ===================================================== BSP buttons */

static void jump_to_status_async(void *arg)
{
    (void)arg;
    bsp_display_lock(0);
    ui_jump_to_status_tab();
    bsp_display_unlock();
}

static void on_left_button(void *arg, void *data)
{
    (void)arg;
    (void)data;
    lv_async_call(jump_to_status_async, NULL);
}

static void on_right_button(void *arg, void *data)
{
    (void)arg;
    (void)data;
    if (state_get() != APP_STATE_NEEDS_INIT) {
        schedule_toast("Initialize only available in NEEDS_INIT");
        return;
    }
    pump_cmd_t cmd = {
        .kind = PUMP_CMD_INITIALIZE,
        .payload.init = {.force = 2, .ccw = false},
    };
    if (!pump_task_enqueue(&cmd)) {
        schedule_toast("Pump busy");
    }
}

static void wire_bsp_buttons(void)
{
    /* esp-box-3 BSP exposes button handles via bsp_iot_button_create.
     * The exact API depends on BSP version — left button = index 0,
     * right = 1 in the v4 layout. We register single-click callbacks. */
    button_handle_t btns[BSP_BUTTON_NUM] = {0};
    ESP_ERROR_CHECK(bsp_iot_button_create(btns, NULL, BSP_BUTTON_NUM));
    if (BSP_BUTTON_NUM >= 1 && btns[0] != NULL) {
        iot_button_register_cb(btns[0], BUTTON_SINGLE_CLICK, NULL,
                               on_left_button, NULL);
    }
    if (BSP_BUTTON_NUM >= 2 && btns[1] != NULL) {
        iot_button_register_cb(btns[1], BUTTON_SINGLE_CLICK, NULL,
                               on_right_button, NULL);
    }
}

/* ===================================================== app_main */

void app_main(void)
{
    ESP_LOGI(TAG, "sy01b-client booting (Phase C, motion UI)");

    state_init();
    ESP_ERROR_CHECK(config_store_init());

    bsp_i2c_init();
    bsp_display_start();
    bsp_display_lock(0);
    ui_create();
    ui_apply_state(APP_STATE_BOOT, "Booting", false);
    bsp_display_unlock();
    bsp_display_backlight_on();

    wire_bsp_buttons();

    schedule_ui_refresh();
    esp_err_t err = wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi never came up");
        state_set_error_recoverable("WiFi failed — check Kconfig SSID/PSK");
        schedule_ui_refresh();
        return;
    }

    err = pump_client_init();
    if (err != ESP_OK) {
        state_set_error_recoverable("server URL unset");
        schedule_ui_refresh();
        return;
    }

    if (run_diagnose_once() != ESP_OK) {
        return;
    }

    pump_task_start();
    xTaskCreate(status_task, "status_task", 6 * 1024, NULL, 4, NULL);
}
