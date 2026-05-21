#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "bsp/esp-box-3.h"

#include "config_store.h"
#include "pump_client.h"
#include "state.h"
#include "ui.h"
#include "wifi.h"

static const char *TAG = "main";

static void apply_ui_state(void *unused)
{
    (void)unused;
    app_state_t s = state_get();
    app_status_t snap;
    state_snapshot(&snap);
    const char *text;
    switch (s) {
    case APP_STATE_BOOT:
        text = "Booting";
        break;
    case APP_STATE_WIFI_CONNECTING:
        text = "Connecting WiFi";
        break;
    case APP_STATE_DIAGNOSING:
        text = "Diagnosing";
        break;
    case APP_STATE_NEEDS_INIT:
        text = "Needs Initialize";
        break;
    case APP_STATE_WIFI_LOST:
        text = snap.error_msg;
        break;
    case APP_STATE_ERROR_RECOVERABLE:
        text = snap.error_msg;
        break;
    default:
        text = "Unknown";
        break;
    }
    bsp_display_lock(0);
    ui_apply_state(s, text);
    ui_apply_status(&snap);
    bsp_display_unlock();
}

static void schedule_ui_refresh(void)
{
    lv_async_call(apply_ui_state, NULL);
}

/**
 * Background task: GET /v1/status every CONFIG_PUMP_STATUS_POLL_INTERVAL_MS
 * and push the result into state_update_status, then ask the LVGL
 * thread to redraw. Phase B never calls any motion endpoint.
 */
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

static esp_err_t run_diagnose_once(void)
{
    pump_diag_t diag;
    state_set_diagnosing();
    schedule_ui_refresh();
    esp_err_t err = pump_diagnose(&diag);
    if (err != ESP_OK) {
        state_set_error("diagnose failed — check server");
        schedule_ui_refresh();
        return err;
    }
    if (!diag.ok_to_initialize) {
        ESP_LOGW(TAG, "diagnose says NOT ok_to_initialize");
    }
    state_set_needs_init(diag.software_version, diag.supply_volts);
    /* Seed the cached status from the diagnose payload so the Status
     * tab is populated before the first /v1/status poll completes. */
    state_update_status(diag.valve_position, diag.plunger_steps, false,
                        diag.pre_init_error_name, diag.pre_init_error_code);
    schedule_ui_refresh();
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "sy01b-client booting (Phase B, read-only)");

    state_init();
    ESP_ERROR_CHECK(config_store_init());

    bsp_i2c_init();
    bsp_display_start();
    bsp_display_lock(0);
    ui_create();
    ui_apply_state(APP_STATE_BOOT, "Booting");
    bsp_display_unlock();
    bsp_display_backlight_on();

    /* WiFi blocks until first IP. Initial state moves to
     * WIFI_CONNECTING before; the wifi event handler bumps to
     * DIAGNOSING via state_set_wifi_connected. */
    schedule_ui_refresh();
    esp_err_t err = wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi never came up");
        state_set_error("WiFi failed — check Kconfig SSID/PSK");
        schedule_ui_refresh();
        return;
    }

    err = pump_client_init();
    if (err != ESP_OK) {
        state_set_error("server URL unset");
        schedule_ui_refresh();
        return;
    }

    if (run_diagnose_once() != ESP_OK) {
        return;
    }

    /* Status tab refresher. Phase B has no motion task. */
    xTaskCreate(status_task, "status_task", 6 * 1024, NULL, 5, NULL);
}
