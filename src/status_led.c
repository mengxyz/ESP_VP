#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_vp.h"

static const char *TAG = "status_led";

static rmt_channel_handle_t s_channel;
static rmt_encoder_handle_t s_encoder;
static volatile esp_vp_status_t s_status = ESP_VP_STATUS_BOOT;
static volatile esp_vp_status_t s_pulse_status = ESP_VP_STATUS_BOOT;
static volatile TickType_t s_pulse_until;
static bool s_enabled;

static uint8_t scale(uint8_t value, uint8_t brightness)
{
    if (brightness > 48) {
        brightness = 48;
    }
    return (uint8_t)(((uint16_t)value * brightness) / 255);
}

static void set_rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness)
{
    if (!s_enabled || s_channel == NULL || s_encoder == NULL) {
        return;
    }
    uint8_t grb[3] = {
        scale(g, brightness),
        scale(r, brightness),
        scale(b, brightness),
    };
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    esp_err_t err = rmt_transmit(s_channel, s_encoder, grb, sizeof(grb), &tx_config);
    if (err == ESP_OK) {
        rmt_tx_wait_all_done(s_channel, 20);
    }
}

static void show_pattern(esp_vp_status_t status, uint32_t tick)
{
    switch (status) {
    case ESP_VP_STATUS_BOOT: {
        uint8_t pulse[] = {12, 24, 42, 70, 42, 24};
        set_rgb(40, 90, 255, pulse[tick % 6]);
        break;
    }
    case ESP_VP_STATUS_WIFI_CONNECTING: {
        uint8_t pulse[] = {10, 35, 90, 35};
        set_rgb(255, 120, 0, pulse[tick % 4]);
        break;
    }
    case ESP_VP_STATUS_READY:
        if ((tick % 50) == 0 || (tick % 50) == 3) {
            set_rgb(0, 255, 90, 100);
        } else {
            set_rgb(0, 255, 60, 10);
        }
        break;
    case ESP_VP_STATUS_CLIENT_ACTIVE:
        set_rgb(0, 210, 255, (tick % 2) ? 160 : 35);
        break;
    case ESP_VP_STATUS_UPLOADING:
        if ((tick % 6) < 3) {
            set_rgb(180, 40, 255, 150);
        } else {
            set_rgb(0, 180, 255, 90);
        }
        break;
    case ESP_VP_STATUS_PAIRING:
        set_rgb(0, 220, 255, (tick % 4) < 2 ? 140 : 12);
        break;
    case ESP_VP_STATUS_ERROR:
    default:
        set_rgb(255, 0, 0, (tick % 2) ? 180 : 12);
        break;
    }
}

static void status_led_task(void *arg)
{
    uint32_t tick = 0;
    while (true) {
        TickType_t now = xTaskGetTickCount();
        esp_vp_status_t status = now < s_pulse_until ? s_pulse_status : s_status;
        show_pattern(status, tick++);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void status_led_init(void)
{
#if APP_STATUS_LED_PIN >= 0
    rmt_tx_channel_config_t channel_config = {
        .gpio_num = APP_STATUS_LED_PIN,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
        .flags = {
            .invert_out = 0,
            .with_dma = 0,
        },
    };
    esp_err_t err = rmt_new_tx_channel(&channel_config, &s_channel);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "disabled: GPIO%d RMT channel init failed: %s", APP_STATUS_LED_PIN, esp_err_to_name(err));
        return;
    }
    rmt_bytes_encoder_config_t encoder_config = {
        .bit0 = {
            .duration0 = 3,
            .level0 = 1,
            .duration1 = 9,
            .level1 = 0,
        },
        .bit1 = {
            .duration0 = 9,
            .level0 = 1,
            .duration1 = 3,
            .level1 = 0,
        },
        .flags = {
            .msb_first = 1,
        },
    };
    err = rmt_new_bytes_encoder(&encoder_config, &s_encoder);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "disabled: GPIO%d RMT encoder init failed: %s", APP_STATUS_LED_PIN, esp_err_to_name(err));
        return;
    }
    err = rmt_enable(s_channel);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "disabled: GPIO%d RMT enable failed: %s", APP_STATUS_LED_PIN, esp_err_to_name(err));
        return;
    }
    s_enabled = true;
    ESP_LOGI(TAG, "enabled on GPIO%d", APP_STATUS_LED_PIN);
    xTaskCreate(status_led_task, "status_led", 3072, NULL, 3, NULL);
#else
    ESP_LOGI(TAG, "disabled; pass --status-led-pin <gpio> to enable");
#endif
}

void status_led_set(esp_vp_status_t status)
{
    s_status = status;
}

void status_led_pulse(esp_vp_status_t status, uint32_t duration_ms)
{
    s_pulse_status = status;
    s_pulse_until = xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms);
}
