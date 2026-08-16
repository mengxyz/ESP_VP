#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_vp.h"

static const char *TAG = "wifi";
static EventGroupHandle_t s_wifi_events;
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_SOFTAP_BIT = BIT1;
static const int WIFI_FAILED_BIT = BIT2;
static const int WIFI_MAX_RETRIES = 10;
static char s_local_ip[16] = "0.0.0.0";
static char s_softap_ssid[33] = "";
static bool s_softap_active = false;
static int s_retry_count = 0;

static bool has_sta_ssid(const char *ssid)
{
    return ssid != NULL && ssid[0] != '\0' && strcmp(ssid, "CHANGE_ME") != 0 && strcmp(ssid, "YOUR_WIFI") != 0;
}

static void load_sta_config(char *ssid, size_t ssid_len, char *password, size_t password_len)
{
    strlcpy(ssid, APP_WIFI_SSID, ssid_len);
    strlcpy(password, APP_WIFI_PASSWORD, password_len);

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return;
    }
    size_t len = ssid_len;
    err = nvs_get_str(nvs, "ssid", ssid, &len);
    if (err == ESP_OK) {
        len = password_len;
        err = nvs_get_str(nvs, "password", password, &len);
        if (err != ESP_OK) {
            password[0] = '\0';
        }
        ESP_LOGI(TAG, "loaded Wi-Fi credentials from NVS ssid=\"%s\"", ssid);
    }
    nvs_close(nvs);
}

esp_err_t wifi_save_sta_config(const char *ssid, const char *password)
{
    if (!has_sta_ssid(ssid)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "password", password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved Wi-Fi credentials ssid=\"%s\"; rebooting is required to connect", ssid);
    }
    return err;
}

bool wifi_is_softap_active(void)
{
    return s_softap_active;
}

const char *wifi_softap_ssid(void)
{
    return s_softap_ssid;
}

static void start_softap(bool keep_sta)
{
    if (s_softap_active) {
        xEventGroupSetBits(s_wifi_events, WIFI_SOFTAP_BIT);
        return;
    }

    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(s_softap_ssid, sizeof(s_softap_ssid), "ESP-VP-%02X%02X%02X", mac[3], mac[4], mac[5]);

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, s_softap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(s_softap_ssid);
    ap_config.ap.password[0] = '\0';
    ap_config.ap.channel = 6;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(keep_sta ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    s_softap_active = true;
    strlcpy(s_local_ip, "192.168.4.1", sizeof(s_local_ip));
    status_led_set(ESP_VP_STATUS_WIFI_CONNECTING);
    ESP_LOGW(TAG, "STA not connected; open SoftAP provisioning active ssid=\"%s\" url=http://192.168.4.1:%d", s_softap_ssid, ESP_VP_MANAGEMENT_PORT);
    xEventGroupSetBits(s_wifi_events, WIFI_SOFTAP_BIT);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        status_led_set(ESP_VP_STATUS_WIFI_CONNECTING);
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_retry_count++;
        ESP_LOGW(TAG, "disconnected, reconnecting attempt=%d/%d", s_retry_count, WIFI_MAX_RETRIES);
        status_led_set(ESP_VP_STATUS_WIFI_CONNECTING);
        if (s_retry_count >= WIFI_MAX_RETRIES) {
            xEventGroupSetBits(s_wifi_events, WIFI_FAILED_BIT);
            start_softap(true);
        } else {
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_retry_count = 0;
        snprintf(s_local_ip, sizeof(s_local_ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&event->ip_info.ip));
        status_led_set(ESP_VP_STATUS_READY);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

const char *wifi_local_ip(void)
{
    return s_local_ip;
}

esp_err_t wifi_start(void)
{
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    char ssid[33] = "";
    char password[65] = "";
    load_sta_config(ssid, sizeof(ssid), password, sizeof(password));

    esp_err_t tx_power_err = esp_wifi_set_max_tx_power(52); /* 13 dBm, lower peak current during association. */
    if (tx_power_err != ESP_OK) {
        ESP_LOGW(TAG, "failed to limit TX power: %s", esp_err_to_name(tx_power_err));
    }

    if (!has_sta_ssid(ssid)) {
        ESP_LOGW(TAG, "no Wi-Fi SSID configured; starting SoftAP provisioning");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_start());
        start_softap(false);
        return ESP_OK;
    }

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_SOFTAP_BIT | WIFI_FAILED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(45000));
    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    if (!(bits & WIFI_SOFTAP_BIT)) {
        ESP_LOGW(TAG, "Wi-Fi connection timed out; starting SoftAP provisioning");
        start_softap(true);
    }
    return ESP_OK;
}
