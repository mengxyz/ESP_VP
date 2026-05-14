#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"

#include "esp_vp.h"

static const char *TAG = "device_config";

typedef struct {
    bool configured;
    char name[64];
    char model_code[16];
    char product_name[32];
    char serial[32];
    char access_code[16];
    char mode[24];
    int paired_printer_id;
    char upload_base_url[128];
    char api_key[96];
    char receiver_token[96];
    char tls_cert_pem[4096];
    char tls_key_pem[4096];
} runtime_config_t;

static runtime_config_t s_config;
static char s_device_id[32] = ESP_VP_SERIAL;

static bool json_get_string(const char *json, const char *key, char *out, size_t out_len);

static void init_device_id(void)
{
    if (!esp_vp_manager_mode()) {
        strlcpy(s_device_id, ESP_VP_SERIAL, sizeof(s_device_id));
        return;
    }

    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to read Wi-Fi MAC for device id err=%s; using %s",
                 esp_err_to_name(err), ESP_VP_SERIAL);
        strlcpy(s_device_id, ESP_VP_SERIAL, sizeof(s_device_id));
        return;
    }

    snprintf(s_device_id, sizeof(s_device_id), "VP%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void set_default_config(void)
{
    init_device_id();
    s_config.configured = !esp_vp_manager_mode();
    strlcpy(s_config.name, APP_VP_NAME, sizeof(s_config.name));
    strlcpy(s_config.model_code, APP_VP_MODEL_CODE, sizeof(s_config.model_code));
    strlcpy(s_config.product_name, APP_VP_PRODUCT_NAME, sizeof(s_config.product_name));
    strlcpy(s_config.serial, ESP_VP_SERIAL, sizeof(s_config.serial));
    strlcpy(s_config.access_code, APP_VP_ACCESS_CODE, sizeof(s_config.access_code));
    strlcpy(s_config.mode, "archive", sizeof(s_config.mode));
    s_config.paired_printer_id = 0;
    strlcpy(s_config.upload_base_url, APP_BAMBUDDY_BASE_URL, sizeof(s_config.upload_base_url));
    strlcpy(s_config.api_key, APP_BAMBUDDY_API_KEY, sizeof(s_config.api_key));
    strlcpy(s_config.receiver_token, "", sizeof(s_config.receiver_token));
    strlcpy(s_config.tls_cert_pem, APP_TLS_CERT_PEM, sizeof(s_config.tls_cert_pem));
    strlcpy(s_config.tls_key_pem, APP_TLS_KEY_PEM, sizeof(s_config.tls_key_pem));
}

static void load_str(nvs_handle_t nvs, const char *key, char *out, size_t out_len)
{
    size_t len = out_len;
    esp_err_t err = nvs_get_str(nvs, key, out, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "failed to load %s err=%s", key, esp_err_to_name(err));
    }
}

static void save_str(nvs_handle_t nvs, const char *key, const char *value)
{
    esp_err_t err = nvs_set_str(nvs, key, value ? value : "");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to save %s err=%s", key, esp_err_to_name(err));
    }
}

esp_err_t esp_vp_config_init(void)
{
    set_default_config();
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("esp_vp", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed err=%s; using build config", esp_err_to_name(err));
        return err;
    }
    load_str(nvs, "name", s_config.name, sizeof(s_config.name));
    load_str(nvs, "model", s_config.model_code, sizeof(s_config.model_code));
    load_str(nvs, "product", s_config.product_name, sizeof(s_config.product_name));
    load_str(nvs, "serial", s_config.serial, sizeof(s_config.serial));
    load_str(nvs, "access", s_config.access_code, sizeof(s_config.access_code));
    load_str(nvs, "mode", s_config.mode, sizeof(s_config.mode));
    load_str(nvs, "upload_url", s_config.upload_base_url, sizeof(s_config.upload_base_url));
    load_str(nvs, "api_key", s_config.api_key, sizeof(s_config.api_key));
    load_str(nvs, "token", s_config.receiver_token, sizeof(s_config.receiver_token));
    load_str(nvs, "tls_cert", s_config.tls_cert_pem, sizeof(s_config.tls_cert_pem));
    load_str(nvs, "tls_key", s_config.tls_key_pem, sizeof(s_config.tls_key_pem));
    uint8_t configured = s_config.configured ? 1 : 0;
    err = nvs_get_u8(nvs, "configured", &configured);
    if (err == ESP_OK) {
        s_config.configured = configured != 0;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "failed to load configured err=%s", esp_err_to_name(err));
    }
    int32_t paired_printer_id = s_config.paired_printer_id;
    err = nvs_get_i32(nvs, "paired_pid", &paired_printer_id);
    if (err == ESP_OK) {
        s_config.paired_printer_id = (int)paired_printer_id;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "failed to load paired_pid err=%s", esp_err_to_name(err));
    }
    nvs_close(nvs);
    return ESP_OK;
}

const char *esp_vp_firmware_version(void) { return APP_ESP_VP_FIRMWARE_VERSION; }
bool esp_vp_manager_mode(void) { return APP_MANAGER_MODE != 0; }
bool esp_vp_is_configured(void) { return s_config.configured; }
bool esp_vp_is_paired(void) { return s_config.receiver_token[0] != '\0'; }
const char *esp_vp_device_id(void) { return s_device_id; }
const char *esp_vp_name(void) { return s_config.name; }
const char *esp_vp_model_code(void) { return s_config.model_code; }
const char *esp_vp_product_name(void) { return s_config.product_name; }
const char *esp_vp_serial(void) { return s_config.serial; }
const char *esp_vp_access_code(void) { return s_config.access_code; }
const char *esp_vp_mode(void) { return s_config.mode; }
int esp_vp_paired_printer_id(void) { return s_config.paired_printer_id; }
const char *esp_vp_api_key(void) { return s_config.api_key; }
const char *esp_vp_receiver_token(void) { return s_config.receiver_token; }
const char *esp_vp_tls_cert_pem(void) { return s_config.tls_cert_pem; }
const char *esp_vp_tls_key_pem(void) { return s_config.tls_key_pem; }
const char *esp_vp_upload_base_url(void) { return s_config.upload_base_url; }

void esp_vp_set_upload_base_url(const char *url)
{
    if (url == NULL || strncmp(url, "http://", 7) != 0) {
        return;
    }
    if (strcmp(s_config.upload_base_url, url) == 0) {
        return;
    }
    strlcpy(s_config.upload_base_url, url, sizeof(s_config.upload_base_url));
    nvs_handle_t nvs;
    if (nvs_open("esp_vp", NVS_READWRITE, &nvs) == ESP_OK) {
        save_str(nvs, "upload_url", s_config.upload_base_url);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "manager upload URL set to %s", s_config.upload_base_url);
}

esp_err_t esp_vp_pair_json(const char *json)
{
    char upload_url[sizeof(s_config.upload_base_url)];
    char token[sizeof(s_config.receiver_token)];

    strlcpy(upload_url, s_config.upload_base_url, sizeof(upload_url));
    strlcpy(token, s_config.receiver_token, sizeof(token));

    if (!json_get_string(json, "receiver_url", upload_url, sizeof(upload_url))) {
        json_get_string(json, "upload_url", upload_url, sizeof(upload_url));
    }
    json_get_string(json, "receiver_token", token, sizeof(token));

    if (strncmp(upload_url, "http://", 7) != 0 || token[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_config.upload_base_url, upload_url, sizeof(s_config.upload_base_url));
    strlcpy(s_config.receiver_token, token, sizeof(s_config.receiver_token));

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("esp_vp", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    save_str(nvs, "upload_url", s_config.upload_base_url);
    save_str(nvs, "token", s_config.receiver_token);
    err = nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "paired with receiver upload_base_url=%s", s_config.upload_base_url);
    return err;
}

static bool json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    char pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (p == NULL) {
        return false;
    }
    p = strchr(p + strlen(pattern), ':');
    if (p == NULL) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    size_t idx = 0;
    while (*p != '\0' && *p != '"' && idx + 1 < out_len) {
        if (*p == '\\' && p[1] != '\0') {
            p++;
            if (*p == 'n') {
                out[idx++] = '\n';
                p++;
                continue;
            }
            if (*p == 'r') {
                out[idx++] = '\r';
                p++;
                continue;
            }
            if (*p == 't') {
                out[idx++] = '\t';
                p++;
                continue;
            }
        }
        out[idx++] = *p++;
    }
    out[idx] = '\0';
    return idx > 0;
}

esp_err_t esp_vp_apply_config_json(const char *json)
{
    char name[sizeof(s_config.name)];
    char model[sizeof(s_config.model_code)];
    char product[sizeof(s_config.product_name)];
    char serial[sizeof(s_config.serial)];
    char access[sizeof(s_config.access_code)];
    char mode[sizeof(s_config.mode)];
    char upload_url[sizeof(s_config.upload_base_url)];
    char api_key[sizeof(s_config.api_key)];
    char token[sizeof(s_config.receiver_token)];
    char tls_cert[sizeof(s_config.tls_cert_pem)];
    char tls_key[sizeof(s_config.tls_key_pem)];

    strlcpy(name, s_config.name, sizeof(name));
    strlcpy(model, s_config.model_code, sizeof(model));
    strlcpy(product, s_config.product_name, sizeof(product));
    strlcpy(serial, s_config.serial, sizeof(serial));
    strlcpy(access, s_config.access_code, sizeof(access));
    strlcpy(mode, s_config.mode, sizeof(mode));
    strlcpy(upload_url, s_config.upload_base_url, sizeof(upload_url));
    strlcpy(api_key, s_config.api_key, sizeof(api_key));
    strlcpy(token, s_config.receiver_token, sizeof(token));
    strlcpy(tls_cert, s_config.tls_cert_pem, sizeof(tls_cert));
    strlcpy(tls_key, s_config.tls_key_pem, sizeof(tls_key));

    json_get_string(json, "name", name, sizeof(name));
    json_get_string(json, "model_code", model, sizeof(model));
    json_get_string(json, "product_name", product, sizeof(product));
    json_get_string(json, "serial", serial, sizeof(serial));
    json_get_string(json, "access_code", access, sizeof(access));
    json_get_string(json, "mode", mode, sizeof(mode));
    if (!json_get_string(json, "receiver_url", upload_url, sizeof(upload_url))) {
        if (json_get_string(json, "upload_url", upload_url, sizeof(upload_url))) {
            const char *suffix = "/api/v1/esp-vp/upload";
            char *found = strstr(upload_url, suffix);
            if (found != NULL && found[strlen(suffix)] == '\0') {
                *found = '\0';
            }
        }
    }
    json_get_string(json, "api_key", api_key, sizeof(api_key));
    json_get_string(json, "receiver_token", token, sizeof(token));
    json_get_string(json, "tls_cert_pem", tls_cert, sizeof(tls_cert));
    json_get_string(json, "tls_key_pem", tls_key, sizeof(tls_key));

    if (strncmp(upload_url, "http://", 7) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_config.name, name, sizeof(s_config.name));
    strlcpy(s_config.model_code, model, sizeof(s_config.model_code));
    strlcpy(s_config.product_name, product, sizeof(s_config.product_name));
    strlcpy(s_config.serial, serial, sizeof(s_config.serial));
    strlcpy(s_config.access_code, access, sizeof(s_config.access_code));
    strlcpy(s_config.mode, mode, sizeof(s_config.mode));
    strlcpy(s_config.upload_base_url, upload_url, sizeof(s_config.upload_base_url));
    strlcpy(s_config.api_key, api_key, sizeof(s_config.api_key));
    strlcpy(s_config.receiver_token, token, sizeof(s_config.receiver_token));
    strlcpy(s_config.tls_cert_pem, tls_cert, sizeof(s_config.tls_cert_pem));
    strlcpy(s_config.tls_key_pem, tls_key, sizeof(s_config.tls_key_pem));
    s_config.configured = true;
    const char *paired_key = strstr(json, "\"paired_printer_id\"");
    if (paired_key == NULL) {
        paired_key = strstr(json, "\"target_printer_id\"");
    }
    if (paired_key == NULL) {
        paired_key = strstr(json, "\"printer_id\"");
    }
    if (paired_key != NULL) {
        const char *colon = strchr(paired_key, ':');
        if (colon != NULL) {
            s_config.paired_printer_id = atoi(colon + 1);
        }
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("esp_vp", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    save_str(nvs, "name", s_config.name);
    save_str(nvs, "model", s_config.model_code);
    save_str(nvs, "product", s_config.product_name);
    save_str(nvs, "serial", s_config.serial);
    save_str(nvs, "access", s_config.access_code);
    save_str(nvs, "mode", s_config.mode);
    save_str(nvs, "upload_url", s_config.upload_base_url);
    save_str(nvs, "api_key", s_config.api_key);
    save_str(nvs, "token", s_config.receiver_token);
    save_str(nvs, "tls_cert", s_config.tls_cert_pem);
    save_str(nvs, "tls_key", s_config.tls_key_pem);
    esp_err_t save_err = nvs_set_u8(nvs, "configured", s_config.configured ? 1 : 0);
    if (save_err != ESP_OK) {
        ESP_LOGW(TAG, "failed to save configured err=%s", esp_err_to_name(save_err));
    }
    save_err = nvs_set_i32(nvs, "paired_pid", s_config.paired_printer_id);
    if (save_err != ESP_OK) {
        ESP_LOGW(TAG, "failed to save paired_pid err=%s", esp_err_to_name(save_err));
    }
    err = nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "applied config name=\"%s\" model=%s product=\"%s\" serial=%s mode=%s paired_printer_id=%d upload_base_url=%s",
             s_config.name, s_config.model_code, s_config.product_name, s_config.serial,
             s_config.mode, s_config.paired_printer_id, s_config.upload_base_url);
    if (err == ESP_OK) {
        esp_vp_start_printer_services_once();
    }
    return err;
}
