#pragma once

/* Defaults used when esp-vp/build.py has not generated app_config.generated.h. */
#if __has_include("app_config.generated.h")
#include "app_config.generated.h"
#endif

#ifndef APP_WIFI_SSID
#define APP_WIFI_SSID "CHANGE_ME"
#endif
#ifndef APP_WIFI_PASSWORD
#define APP_WIFI_PASSWORD "CHANGE_ME"
#endif
#ifndef APP_BAMBUDDY_BASE_URL
#define APP_BAMBUDDY_BASE_URL "http://127.0.0.1:8000"
#endif
#ifndef APP_BAMBUDDY_API_KEY
#define APP_BAMBUDDY_API_KEY ""
#endif
#ifndef APP_MANAGER_MODE
#define APP_MANAGER_MODE 0
#endif
#ifndef APP_ESP_VP_FIRMWARE_VERSION
#define APP_ESP_VP_FIRMWARE_VERSION "esp-vp-manager-discovery-2026-06-24"
#endif
#ifndef APP_RECEIVER_ENROLLMENT_KEY
#define APP_RECEIVER_ENROLLMENT_KEY APP_BAMBUDDY_API_KEY
#endif
#ifndef APP_STATUS_LED_PIN
#define APP_STATUS_LED_PIN -1
#endif
#ifndef APP_PAIR_BUTTON_PIN
#define APP_PAIR_BUTTON_PIN 0
#endif
#ifndef APP_VP_NAME
#define APP_VP_NAME "Bambuddy ESP VP"
#endif
#ifndef APP_VP_MODEL_CODE
#define APP_VP_MODEL_CODE "BL-P001"
#endif
#ifndef APP_VP_PRODUCT_NAME
#define APP_VP_PRODUCT_NAME "X1 Carbon"
#endif
#ifndef APP_VP_ACCESS_CODE
#define APP_VP_ACCESS_CODE "12345678"
#endif
#ifndef APP_VP_SERIAL_SUFFIX
#define APP_VP_SERIAL_SUFFIX "391800001"
#endif
#ifndef APP_TLS_CERT_PEM
#define APP_TLS_CERT_PEM ""
#endif
#ifndef APP_TLS_KEY_PEM
#define APP_TLS_KEY_PEM ""
#endif
