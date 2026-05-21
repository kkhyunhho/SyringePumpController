#include "pump_client.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "cJSON.h"

#include "config_store.h"

static const char *TAG = "pump_client";

#define MAX_URL_LEN  192
#define MAX_RESP_LEN 2048
#define MAX_PATH_LEN 32

static char s_base_url[MAX_URL_LEN];

esp_err_t pump_client_init(void)
{
    esp_err_t err = config_store_get_server_url(s_base_url, sizeof(s_base_url));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no server URL configured");
        return err;
    }
    ESP_LOGI(TAG, "server URL: %s", s_base_url);
    return ESP_OK;
}

/**
 * Issue a single HTTP request and read up to ``out_cap - 1`` bytes
 * of the response into ``out`` (null-terminated). ``method`` is one
 * of HTTP_METHOD_GET or HTTP_METHOD_POST; ``body`` may be NULL for
 * GET requests. Returns ESP_OK on HTTP 2xx, ESP_FAIL on non-2xx,
 * or the underlying transport ``esp_err_t``.
 */
static esp_err_t http_perform(esp_http_client_method_t method, const char *path,
                              const char *body, char *out, size_t out_cap)
{
    char url[MAX_URL_LEN + MAX_PATH_LEN];
    snprintf(url, sizeof(url), "%s%s", s_base_url, path);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = CONFIG_PUMP_HTTP_TIMEOUT_MS,
        .method = method,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    if (body != NULL) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, strlen(body));
    }

    err = esp_http_client_open(client, body != NULL ? (int)strlen(body) : 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open %s: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    if (body != NULL) {
        int written = esp_http_client_write(client, body, (int)strlen(body));
        if (written < 0) {
            ESP_LOGE(TAG, "http write %s failed", url);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
    }

    int headers = esp_http_client_fetch_headers(client);
    (void)headers;
    int status = esp_http_client_get_status_code(client);

    int total = 0;
    while (total < (int)(out_cap - 1)) {
        int n = esp_http_client_read(client, out + total,
                                     (int)(out_cap - 1 - total));
        if (n <= 0) {
            break;
        }
        total += n;
    }
    out[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "%s returned HTTP %d: %s", url, status, out);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void copy_json_str(cJSON *parent, const char *field, char *dst,
                          size_t cap)
{
    if (cap == 0) {
        return;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, field);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strncpy(dst, item->valuestring, cap - 1);
        dst[cap - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

static int json_int(cJSON *parent, const char *field, int def)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, field);
    if (cJSON_IsNumber(item)) {
        return (int)item->valuedouble;
    }
    return def;
}

static float json_float(cJSON *parent, const char *field, float def)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, field);
    if (cJSON_IsNumber(item)) {
        return (float)item->valuedouble;
    }
    return def;
}

static bool json_bool(cJSON *parent, const char *field, bool def)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, field);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return def;
}

esp_err_t pump_diagnose(pump_diag_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char resp[MAX_RESP_LEN];
    esp_err_t err =
        http_perform(HTTP_METHOD_GET, "/v1/diagnose", NULL, resp, sizeof(resp));
    if (err != ESP_OK) {
        return err;
    }
    cJSON *root = cJSON_Parse(resp);
    if (root == NULL) {
        ESP_LOGE(TAG, "diagnose: JSON parse failed");
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(out, 0, sizeof(*out));
    out->ok_to_initialize = json_bool(root, "ok_to_initialize", false);
    out->supply_volts = json_float(root, "supply_volts", 0.0f);
    out->plunger_steps = json_int(root, "plunger_steps", 0);
    out->pre_init_error_code = json_int(root, "pre_init_error_code", -1);
    copy_json_str(root, "software_version", out->software_version,
                  sizeof(out->software_version));
    copy_json_str(root, "serial_number", out->serial_number,
                  sizeof(out->serial_number));
    copy_json_str(root, "valve_position", out->valve_position,
                  sizeof(out->valve_position));
    copy_json_str(root, "pre_init_error_name", out->pre_init_error_name,
                  sizeof(out->pre_init_error_name));

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t pump_status(pump_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char resp[MAX_RESP_LEN];
    esp_err_t err =
        http_perform(HTTP_METHOD_GET, "/v1/status", NULL, resp, sizeof(resp));
    if (err != ESP_OK) {
        return err;
    }
    cJSON *root = cJSON_Parse(resp);
    if (root == NULL) {
        ESP_LOGE(TAG, "status: JSON parse failed");
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(out, 0, sizeof(*out));
    copy_json_str(root, "valve", out->valve, sizeof(out->valve));
    out->plunger_steps = json_int(root, "plunger_steps", 0);
    out->busy = json_bool(root, "busy", false);
    copy_json_str(root, "error_name", out->error_name, sizeof(out->error_name));
    out->error_code = json_int(root, "error_code", -1);

    cJSON_Delete(root);
    return ESP_OK;
}
