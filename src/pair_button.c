#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_vp.h"

static const char *TAG = "pair_button";
static volatile TickType_t s_pair_until;

bool esp_vp_pair_ready(void)
{
    return xTaskGetTickCount() < s_pair_until;
}

int esp_vp_pair_remaining_seconds(void)
{
    TickType_t now = xTaskGetTickCount();
    if (now >= s_pair_until) {
        return 0;
    }
    return (int)((s_pair_until - now) / configTICK_RATE_HZ);
}

void esp_vp_pair_mode_stop(void)
{
    s_pair_until = 0;
    status_led_set(esp_vp_is_configured() ? ESP_VP_STATUS_READY : ESP_VP_STATUS_BOOT);
}

static void pair_button_task(void *arg)
{
    (void)arg;
    int held_ms = 0;
    bool armed = true;
    while (true) {
        bool pressed = gpio_get_level(APP_PAIR_BUTTON_PIN) == 0;
        if (pressed) {
            held_ms += 100;
            if (armed && held_ms >= 5000) {
                s_pair_until = xTaskGetTickCount() + pdMS_TO_TICKS(120000);
                status_led_set(ESP_VP_STATUS_PAIRING);
                ESP_LOGI(TAG, "pair mode enabled for 120 seconds");
                armed = false;
            }
        } else {
            held_ms = 0;
            armed = true;
        }
        if (s_pair_until != 0 && !esp_vp_pair_ready()) {
            s_pair_until = 0;
            status_led_set(esp_vp_is_configured() ? ESP_VP_STATUS_READY : ESP_VP_STATUS_BOOT);
            ESP_LOGI(TAG, "pair mode expired");
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void pair_button_start(void)
{
#if APP_PAIR_BUTTON_PIN >= 0
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << APP_PAIR_BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "disabled: GPIO%d init failed: %s", APP_PAIR_BUTTON_PIN, esp_err_to_name(err));
        return;
    }
    xTaskCreate(pair_button_task, "pair_button", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "enabled on GPIO%d; hold 5 seconds to pair", APP_PAIR_BUTTON_PIN);
#else
    ESP_LOGI(TAG, "disabled");
#endif
}
