#pragma once

#define APP_WIFI_SSID "YOUR_WIFI"
#define APP_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

#define APP_BAMBUDDY_BASE_URL "http://192.168.1.10:8000"
#define APP_BAMBUDDY_API_KEY "bb_REPLACE_WITH_A_BAMBUDDY_API_KEY"
#define APP_STATUS_LED_PIN -1

#define APP_VP_NAME "Bambuddy ESP VP"
#define APP_VP_MODEL_CODE "BL-P001"
#define APP_VP_ACCESS_CODE "12345678"
#define APP_VP_SERIAL_SUFFIX "391800001"

/* Optional PEM certificate/key for TLS listener ports. Generate a private
 * self-signed printer cert for production instead of shipping these defaults.
 */
#define APP_TLS_CERT_PEM ""
#define APP_TLS_KEY_PEM ""
