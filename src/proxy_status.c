#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_vp.h"

static const char *TAG = "proxy_status";
static char s_report_json[8192];
static bool s_started = false;

const char *proxy_status_report_json(void)
{
    return s_report_json[0] ? s_report_json : NULL;
}

static void manager_base_url(char *out, size_t out_len)
{
    strlcpy(out, esp_vp_upload_base_url(), out_len);
    const char *suffix = "/api/v1/esp-vp/upload";
    char *found = strstr(out, suffix);
    if (found != NULL && found[strlen(suffix)] == '\0') {
        *found = '\0';
    }
    size_t len = strlen(out);
    while (len > 0 && out[len - 1] == '/') {
        out[--len] = '\0';
    }
}

static bool extract_report_object(const char *json, char *out, size_t out_len)
{
    const char *key = strstr(json, "\"report\"");
    if (key == NULL) {
        return false;
    }
    const char *colon = strchr(key, ':');
    if (colon == NULL) {
        return false;
    }
    const char *start = strchr(colon, '{');
    if (start == NULL) {
        return false;
    }
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    const char *p = start;
    while (*p != '\0') {
        char ch = *p;
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
        } else if (ch == '"') {
            in_string = true;
        } else if (ch == '{') {
            depth++;
        } else if (ch == '}') {
            depth--;
            if (depth == 0) {
                size_t len = (size_t)(p - start + 1);
                if (len >= out_len) {
                    ESP_LOGW(TAG, "proxy report too large: %u bytes", (unsigned)len);
                    return false;
                }
                memcpy(out, start, len);
                out[len] = '\0';
                return true;
            }
        }
        p++;
    }
    return false;
}

static void poll_once(void)
{
    char base[160];
    manager_base_url(base, sizeof(base));
    if (strncmp(base, "http://", 7) != 0 || esp_vp_receiver_token()[0] == '\0') {
        return;
    }
    char url[256];
    snprintf(url, sizeof(url), "%s/api/v1/devices/%s/proxy-status", base, esp_vp_device_id());

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 8000,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return;
    }
    char auth[128];
    snprintf(auth, sizeof(auth), "Bearer %s", esp_vp_receiver_token());
    esp_http_client_set_header(client, "Authorization", auth);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "proxy status open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }
    int content_length = esp_http_client_fetch_headers(client);
    (void)content_length;
    int status = esp_http_client_get_status_code(client);
    char response[12288];
    int total = 0;
    while (total < (int)sizeof(response) - 1) {
        int read = esp_http_client_read(client, response + total, sizeof(response) - 1 - total);
        if (read <= 0) {
            break;
        }
        total += read;
    }
    response[total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status >= 400 || total <= 0) {
        ESP_LOGW(TAG, "proxy status poll failed status=%d bytes=%d", status, total);
        return;
    }
    if (extract_report_object(response, s_report_json, sizeof(s_report_json))) {
        ESP_LOGI(TAG, "proxy status cache updated printer_id=%d bytes=%u",
                 esp_vp_paired_printer_id(), (unsigned)strlen(s_report_json));
    } else {
        ESP_LOGW(TAG, "proxy status response did not contain report object");
    }
}

static void proxy_status_task(void *arg)
{
    (void)arg;
    while (true) {
        if (esp_vp_is_configured() &&
            strcmp(esp_vp_mode(), "proxy_status") == 0 &&
            esp_vp_paired_printer_id() > 0) {
            poll_once();
            vTaskDelay(pdMS_TO_TICKS(2000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}

void proxy_status_start(void)
{
    if (s_started) {
        return;
    }
    s_started = true;
    xTaskCreate(proxy_status_task, "proxy_status", 8192, NULL, 4, NULL);
}
