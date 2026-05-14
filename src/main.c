#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "esp_vp.h"

static const char *TAG = "esp_vp";
static bool s_printer_services_started = false;

void esp_vp_start_printer_services_once(void)
{
    if (s_printer_services_started) {
        return;
    }
    bind_server_start();
    mqtt_server_start();
    ftps_server_start();
    s_printer_services_started = true;
    ESP_LOGI(TAG, "printer services started");
}

static void device_info_log_task(void *arg)
{
    (void)arg;
    while (true) {
        ESP_LOGI(TAG,
                 "device_info firmware=%s manager_mode=%d configured=%d ip=%s name=\"%s\" model=%s product=\"%s\" serial=%s access_code=%s mode=%s paired_printer_id=%d upload_base_url=%s",
                 esp_vp_firmware_version(),
                 esp_vp_manager_mode() ? 1 : 0,
                 esp_vp_is_configured() ? 1 : 0,
                 wifi_local_ip(),
                 esp_vp_name(),
                 esp_vp_model_code(),
                 esp_vp_product_name(),
                 esp_vp_serial(),
                 esp_vp_access_code(),
                 esp_vp_mode(),
                 esp_vp_paired_printer_id(),
                 esp_vp_upload_base_url());
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_vp_config_init());

    status_led_init();
    pair_button_start();
    status_led_set(ESP_VP_STATUS_BOOT);
    ESP_LOGI(TAG, "firmware=%s manager_mode=%d configured=%d",
             esp_vp_firmware_version(), esp_vp_manager_mode() ? 1 : 0, esp_vp_is_configured() ? 1 : 0);
    ESP_LOGI(TAG, "config name=\"%s\" model=%s product=\"%s\" serial=%s access_code=%s mode=%s paired_printer_id=%d upload_base_url=%s",
             esp_vp_name(),
             esp_vp_model_code(),
             esp_vp_product_name(),
             esp_vp_serial(),
             esp_vp_access_code(),
             esp_vp_mode(),
             esp_vp_paired_printer_id(),
             esp_vp_upload_base_url());
    ESP_ERROR_CHECK(wifi_start());

    ssdp_start();
    management_server_start();
    proxy_status_start();
    if (esp_vp_is_configured()) {
        esp_vp_start_printer_services_once();
    } else {
        ESP_LOGI(TAG, "waiting for VP Manager config before starting printer services");
    }
    xTaskCreate(device_info_log_task, "device_info_log", 4096, NULL, 3, NULL);
}
