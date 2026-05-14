#pragma once

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

esp_err_t wifi_start(void);
const char *wifi_local_ip(void);
void ssdp_start(void);
void bind_server_start(void);
void mqtt_server_start(void);
void ftps_server_start(void);
