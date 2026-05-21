#include "config_store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "cfg";
static const char *NVS_NS = "cfg";

esp_err_t config_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition stale, erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

/**
 * Copy ``def`` into ``out`` (truncated to ``out_len``). Returns
 * ESP_OK if it fit, ESP_ERR_INVALID_SIZE if the default is too long
 * for the caller's buffer.
 */
static esp_err_t fallback(const char *def, char *out, size_t out_len)
{
    if (out_len == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t need = strlen(def);
    if (need >= out_len) {
        out[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, def, need + 1);
    return ESP_OK;
}

/**
 * Read ``key`` from NVS namespace "cfg" into ``out`` (sized
 * ``out_len``). On any NVS failure, fall back to ``def``.
 */
static esp_err_t read_or_fallback(const char *key, const char *def, char *out,
                                  size_t out_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return fallback(def, out, out_len);
    }
    size_t len = out_len;
    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (err == ESP_OK) {
        return ESP_OK;
    }
    return fallback(def, out, out_len);
}

esp_err_t config_store_get_server_url(char *out, size_t out_len)
{
    return read_or_fallback("server_url", CONFIG_PUMP_SERVER_URL, out, out_len);
}

esp_err_t config_store_get_wifi_ssid(char *out, size_t out_len)
{
    return read_or_fallback("wifi_ssid", CONFIG_PUMP_WIFI_SSID, out, out_len);
}

esp_err_t config_store_get_wifi_password(char *out, size_t out_len)
{
    return read_or_fallback("wifi_password", CONFIG_PUMP_WIFI_PASSWORD, out,
                            out_len);
}
