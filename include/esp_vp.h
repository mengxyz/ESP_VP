#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#include "app_config.h"

#ifndef APP_VP_SERIAL
#define ESP_VP_SERIAL_PREFIX "00M00A"
#define APP_VP_SERIAL ESP_VP_SERIAL_PREFIX APP_VP_SERIAL_SUFFIX
#endif
#define ESP_VP_SERIAL APP_VP_SERIAL

#define ESP_VP_SSDP_PORT 2021
#define ESP_VP_BIND_PORT_PLAIN 3000
#define ESP_VP_BIND_PORT_TLS 3002
#define ESP_VP_MQTT_PORT 8883
#define ESP_VP_FTPS_PORT 990
#define ESP_VP_FTPS_PASSIVE_PORT 50000
#define ESP_VP_FTPS_PASSIVE_PORT_COUNT 10
#define ESP_VP_MANAGEMENT_PORT 8080

typedef enum {
    ESP_VP_STATUS_BOOT = 0,
    ESP_VP_STATUS_WIFI_CONNECTING,
    ESP_VP_STATUS_READY,
    ESP_VP_STATUS_CLIENT_ACTIVE,
    ESP_VP_STATUS_UPLOADING,
    ESP_VP_STATUS_PAIRING,
    ESP_VP_STATUS_ERROR,
} esp_vp_status_t;

esp_err_t wifi_start(void);
const char *wifi_local_ip(void);
esp_err_t esp_vp_config_init(void);
const char *esp_vp_firmware_version(void);
bool esp_vp_manager_mode(void);
bool esp_vp_is_configured(void);
bool esp_vp_is_paired(void);
const char *esp_vp_device_id(void);
const char *esp_vp_name(void);
const char *esp_vp_model_code(void);
const char *esp_vp_product_name(void);
const char *esp_vp_serial(void);
const char *esp_vp_access_code(void);
const char *esp_vp_mode(void);
int esp_vp_paired_printer_id(void);
const char *esp_vp_api_key(void);
const char *esp_vp_receiver_token(void);
const char *esp_vp_tls_cert_pem(void);
const char *esp_vp_tls_key_pem(void);
esp_err_t esp_vp_apply_config_json(const char *json);
void status_led_init(void);
void status_led_set(esp_vp_status_t status);
void status_led_pulse(esp_vp_status_t status, uint32_t duration_ms);
const char *esp_vp_upload_base_url(void);
void esp_vp_set_upload_base_url(const char *url);
esp_err_t esp_vp_pair_json(const char *json);
void pair_button_start(void);
bool esp_vp_pair_ready(void);
int esp_vp_pair_remaining_seconds(void);
void esp_vp_pair_mode_stop(void);
void ssdp_start(void);
void bind_server_start(void);
void mqtt_server_start(void);
void ftps_server_start(void);
void management_server_start(void);
void esp_vp_start_printer_services_once(void);
void proxy_status_start(void);
const char *proxy_status_report_json(void);
