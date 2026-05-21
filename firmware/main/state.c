#include "state.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "state";

static SemaphoreHandle_t s_mtx;
static app_state_t s_state = APP_STATE_BOOT;
static app_status_t s_status;

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (cap == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

void state_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    memset(&s_status, 0, sizeof(s_status));
    copy_str(s_status.valve, sizeof(s_status.valve), "?");
    copy_str(s_status.pump_error_name, sizeof(s_status.pump_error_name),
             "UNKNOWN");
}

static void lock(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
}
static void unlock(void)
{
    xSemaphoreGive(s_mtx);
}

app_state_t state_get(void)
{
    lock();
    app_state_t s = s_state;
    unlock();
    return s;
}

void state_snapshot(app_status_t *out)
{
    lock();
    *out = s_status;
    unlock();
}

void state_set_wifi_connected(void)
{
    lock();
    s_status.wifi_connected = true;
    if (s_state == APP_STATE_WIFI_LOST ||
        s_state == APP_STATE_WIFI_CONNECTING) {
        s_state = APP_STATE_DIAGNOSING;
    }
    unlock();
    ESP_LOGI(TAG, "wifi connected");
}

void state_set_wifi_lost(void)
{
    lock();
    s_status.wifi_connected = false;
    s_state = APP_STATE_WIFI_LOST;
    copy_str(s_status.error_msg, sizeof(s_status.error_msg),
             "WiFi lost — reconnecting");
    unlock();
    ESP_LOGW(TAG, "wifi lost");
}

void state_set_diagnosing(void)
{
    lock();
    s_state = APP_STATE_DIAGNOSING;
    s_status.error_msg[0] = '\0';
    unlock();
}

void state_set_needs_init(const char *software_version, float supply_volts)
{
    lock();
    s_state = APP_STATE_NEEDS_INIT;
    copy_str(s_status.software_version, sizeof(s_status.software_version),
             software_version);
    s_status.supply_volts = supply_volts;
    s_status.error_msg[0] = '\0';
    unlock();
    ESP_LOGI(TAG, "needs init (software %s, supply %.1fV)", software_version,
             supply_volts);
}

void state_set_ready(void)
{
    lock();
    s_state = APP_STATE_READY;
    s_status.busy = false;
    s_status.error_msg[0] = '\0';
    s_status.requires_reinit = false;
    s_status.last_error_name[0] = '\0';
    s_status.last_error_message[0] = '\0';
    s_status.last_error_code = 0;
    unlock();
    ESP_LOGI(TAG, "ready");
}

void state_set_busy(void)
{
    lock();
    s_state = APP_STATE_BUSY;
    s_status.busy = true;
    unlock();
}

void state_set_error_recoverable(const char *msg)
{
    lock();
    s_state = APP_STATE_ERROR_RECOVERABLE;
    s_status.busy = false;
    copy_str(s_status.error_msg, sizeof(s_status.error_msg), msg);
    unlock();
    ESP_LOGW(TAG, "recoverable error: %s", msg);
}

void state_set_error_fatal(const char *msg)
{
    lock();
    s_state = APP_STATE_ERROR_FATAL;
    s_status.busy = false;
    s_status.requires_reinit = true;
    copy_str(s_status.error_msg, sizeof(s_status.error_msg), msg);
    unlock();
    ESP_LOGE(TAG, "fatal error (requires re-init): %s", msg);
}

void state_set_error(const char *msg)
{
    state_set_error_recoverable(msg);
}

void state_update_status(const char *valve, int plunger_steps, bool pump_busy,
                         const char *error_name, int error_code)
{
    lock();
    copy_str(s_status.valve, sizeof(s_status.valve), valve);
    s_status.plunger_steps = plunger_steps;
    s_status.pump_busy = pump_busy;
    copy_str(s_status.pump_error_name, sizeof(s_status.pump_error_name),
             error_name);
    s_status.pump_error_code = error_code;
    unlock();
}

void state_record_pump_error(const pump_error_t *err)
{
    if (err == NULL) {
        return;
    }
    lock();
    copy_str(s_status.last_error_name, sizeof(s_status.last_error_name),
             err->error_name);
    copy_str(s_status.last_error_message, sizeof(s_status.last_error_message),
             err->message);
    s_status.last_error_code = err->code;
    s_status.busy = false;
    if (err->klass == PUMP_ERROR_FATAL) {
        s_state = APP_STATE_ERROR_FATAL;
        s_status.requires_reinit = true;
    } else {
        s_state = APP_STATE_ERROR_RECOVERABLE;
    }
    copy_str(s_status.error_msg, sizeof(s_status.error_msg), err->message);
    unlock();
    if (err->klass == PUMP_ERROR_FATAL) {
        ESP_LOGE(TAG, "fatal pump error: %s (code %d)", err->error_name,
                 err->code);
    } else {
        ESP_LOGW(TAG, "recoverable pump error: %s (code %d)", err->error_name,
                 err->code);
    }
}

bool state_requires_reinit(void)
{
    lock();
    bool r = s_status.requires_reinit;
    unlock();
    return r;
}
