#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_vp.h"

static const char *TAG = "esp_vp";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "starting %s serial=%s model=%s", APP_VP_NAME, ESP_VP_SERIAL, APP_VP_MODEL_CODE);
    ESP_ERROR_CHECK(wifi_start());

    ssdp_start();
    bind_server_start();
    mqtt_server_start();
    ftps_server_start();
}
