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
#define MAX_BODY_LEN 192

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
 * Issue a single HTTP request and read up to ``out_cap - 1`` bytes of
 * the response into ``out`` (null-terminated). ``method`` is one of
 * ``HTTP_METHOD_GET`` or ``HTTP_METHOD_POST``; ``body`` may be NULL
 * for GET requests. If ``status_out`` is non-NULL, it receives the
 * HTTP status code regardless of success/failure.
 *
 * Returns ``ESP_OK`` on HTTP 2xx, ``ESP_FAIL`` on non-2xx (body still
 * populated so the caller can parse an error envelope), or the
 * underlying transport ``esp_err_t``.
 */
static esp_err_t http_perform_ex(esp_http_client_method_t method,
                                 const char *path, const char *body, char *out,
                                 size_t out_cap, int *status_out,
                                 int timeout_ms)
{
    if (status_out != NULL) {
        *status_out = 0;
    }

    char url[MAX_URL_LEN + MAX_PATH_LEN];
    snprintf(url, sizeof(url), "%s%s", s_base_url, path);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
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
    if (status_out != NULL) {
        *status_out = status;
    }

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

/* Convenience wrapper using the default Kconfig timeout — fine for
 * short calls (status, diagnose, valve, single aspirate/dispense).
 * Long-running endpoints (prime) call ``http_perform_ex`` directly
 * with a longer per-call timeout. */
static esp_err_t http_perform(esp_http_client_method_t method, const char *path,
                              const char *body, char *out, size_t out_cap,
                              int *status_out)
{
    return http_perform_ex(method, path, body, out, out_cap, status_out,
                           CONFIG_PUMP_HTTP_TIMEOUT_MS);
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

/**
 * Classify a server-side error class name. Fatal = "must re-init"
 * per CLAUDE.md "Error model". Mirror of the server-side mapping in
 * server/errors.py.
 */
static pump_error_class_t classify_error(const char *name, int code)
{
    (void)code;
    if (name == NULL || name[0] == '\0') {
        return PUMP_ERROR_RECOVERABLE;
    }
    if (strcmp(name, "PlungerOverloadError") == 0 ||
        strcmp(name, "InitFailedError") == 0) {
        return PUMP_ERROR_FATAL;
    }
    return PUMP_ERROR_RECOVERABLE;
}

/**
 * Parse a server JSON error envelope `{error, code, command,
 * raw_reply_hex, message}` from ``body`` into ``err``. Robust to
 * non-JSON bodies (server outages, raw HTML 502s) — those fall back
 * to a generic recoverable HttpError.
 */
static void parse_error_body(int http_status, const char *body,
                             pump_error_t *err)
{
    if (err == NULL) {
        return;
    }
    memset(err, 0, sizeof(*err));
    err->http_status = http_status;
    err->code = -1;
    err->klass = PUMP_ERROR_RECOVERABLE;

    cJSON *root = body != NULL ? cJSON_Parse(body) : NULL;
    if (root == NULL) {
        strncpy(err->error_name, "HttpError", sizeof(err->error_name) - 1);
        snprintf(err->message, sizeof(err->message), "HTTP %d (no JSON body)",
                 http_status);
        return;
    }

    copy_json_str(root, "error", err->error_name, sizeof(err->error_name));
    err->code = json_int(root, "code", -1);
    copy_json_str(root, "command", err->command, sizeof(err->command));
    copy_json_str(root, "message", err->message, sizeof(err->message));
    err->klass = classify_error(err->error_name, err->code);

    cJSON_Delete(root);
}

esp_err_t pump_diagnose(pump_diag_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char resp[MAX_RESP_LEN];
    esp_err_t err = http_perform(HTTP_METHOD_GET, "/v1/diagnose", NULL, resp,
                                 sizeof(resp), NULL);
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
    /* New in this revision: syringe_uL + stroke_steps echoed by the
     * server from its Config so the firmware UI can size sliders
     * without a hard-coded default. Older servers omit them; the
     * fallbacks (125 µL / 12 000 steps) match the historical bench. */
    out->syringe_uL = json_float(root, "syringe_uL", 125.0f);
    out->stroke_steps = json_int(root, "stroke_steps", 12000);
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
    esp_err_t err = http_perform(HTTP_METHOD_GET, "/v1/status", NULL, resp,
                                 sizeof(resp), NULL);
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

/**
 * Shared POST + motion-result-parse helper. ``body`` is the JSON
 * request body. On HTTP 2xx, parses {valve, plunger_steps} into
 * ``out``. On 4xx/5xx, fills ``err`` and returns ESP_FAIL.
 */
static esp_err_t post_motion(const char *path, const char *body,
                             pump_motion_result_t *out, pump_error_t *err)
{
    char resp[MAX_RESP_LEN];
    int status = 0;
    esp_err_t rc =
        http_perform(HTTP_METHOD_POST, path, body, resp, sizeof(resp), &status);
    if (rc == ESP_OK) {
        if (out != NULL) {
            memset(out, 0, sizeof(*out));
            cJSON *root = cJSON_Parse(resp);
            if (root != NULL) {
                copy_json_str(root, "valve", out->valve, sizeof(out->valve));
                out->plunger_steps = json_int(root, "plunger_steps", 0);
                cJSON_Delete(root);
            }
        }
        return ESP_OK;
    }
    if (rc == ESP_FAIL) {
        /* HTTP non-2xx — parse error envelope. */
        parse_error_body(status, resp, err);
        return ESP_FAIL;
    }
    /* Transport-level failure (no body to parse). */
    if (err != NULL) {
        memset(err, 0, sizeof(*err));
        err->http_status = status;
        err->klass = PUMP_ERROR_RECOVERABLE;
        strncpy(err->error_name, "TransportError", sizeof(err->error_name) - 1);
        snprintf(err->message, sizeof(err->message), "%s", esp_err_to_name(rc));
    }
    return rc;
}

esp_err_t pump_initialize(int force, bool ccw, pump_motion_result_t *out,
                          pump_error_t *err)
{
    char body[MAX_BODY_LEN];
    snprintf(body, sizeof(body), "{\"force\":%d,\"ccw\":%s}", force,
             ccw ? "true" : "false");
    return post_motion("/v1/initialize", body, out, err);
}

esp_err_t pump_valve(int port, bool ccw, pump_motion_result_t *out,
                     pump_error_t *err)
{
    char body[MAX_BODY_LEN];
    snprintf(body, sizeof(body), "{\"port\":%d,\"ccw\":%s}", port,
             ccw ? "true" : "false");
    return post_motion("/v1/valve", body, out, err);
}

esp_err_t pump_aspirate(float target_uL, pump_motion_result_t *out,
                        pump_error_t *err)
{
    char body[MAX_BODY_LEN];
    snprintf(body, sizeof(body), "{\"target_uL\":%.3f}", target_uL);
    return post_motion("/v1/aspirate", body, out, err);
}

esp_err_t pump_dispense(float target_uL, pump_motion_result_t *out,
                        pump_error_t *err)
{
    char body[MAX_BODY_LEN];
    snprintf(body, sizeof(body), "{\"target_uL\":%.3f}", target_uL);
    return post_motion("/v1/dispense", body, out, err);
}

esp_err_t pump_move_steps(int steps, pump_motion_result_t *out,
                          pump_error_t *err)
{
    char body[MAX_BODY_LEN];
    snprintf(body, sizeof(body), "{\"steps\":%d}", steps);
    return post_motion("/v1/move_steps", body, out, err);
}

esp_err_t pump_prime(int cycles, int source_port, int sink_port,
                     float volume_uL, pump_prime_result_t *out,
                     pump_error_t *err)
{
    char body[MAX_BODY_LEN];
    snprintf(body, sizeof(body),
             "{\"cycles\":%d,\"source_port\":%d,"
             "\"sink_port\":%d,\"volume_uL\":%.3f}",
             cycles, source_port, sink_port, volume_uL);

    /* Prime is the only long-running endpoint — a full cycle is
     * ~30 s (two full strokes + two valve moves), and the operator
     * can request up to 20 cycles. Bump the per-call timeout well
     * past the worst case so the connection doesn't get reset
     * mid-prime and surface as the "HTTP -1 (no JSON body)" fallback
     * from parse_error_body. 5 min covers 20 × 30 s with margin. */
    int timeout_ms = (cycles > 0 ? cycles : 1) * 45000;
    if (timeout_ms < 60000) {
        timeout_ms = 60000;
    }
    if (timeout_ms > 600000) {
        timeout_ms = 600000;
    }

    char resp[MAX_RESP_LEN];
    int status = 0;
    esp_err_t rc = http_perform_ex(HTTP_METHOD_POST, "/v1/prime", body, resp,
                                   sizeof(resp), &status, timeout_ms);
    if (rc == ESP_OK) {
        if (out != NULL) {
            memset(out, 0, sizeof(*out));
            cJSON *root = cJSON_Parse(resp);
            if (root != NULL) {
                out->cycles_done = json_int(root, "cycles_done", 0);
                out->ul_per_stroke = json_int(root, "ul_per_stroke", 0);
                copy_json_str(root, "final_valve", out->final_valve,
                              sizeof(out->final_valve));
                out->final_plunger = json_int(root, "final_plunger", 0);
                cJSON_Delete(root);
            }
        }
        return ESP_OK;
    }
    if (rc == ESP_FAIL) {
        parse_error_body(status, resp, err);
        return ESP_FAIL;
    }
    if (err != NULL) {
        memset(err, 0, sizeof(*err));
        err->http_status = status;
        err->klass = PUMP_ERROR_RECOVERABLE;
        strncpy(err->error_name, "TransportError", sizeof(err->error_name) - 1);
        snprintf(err->message, sizeof(err->message), "%s", esp_err_to_name(rc));
    }
    return rc;
}
