#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_vp.h"

static const char *TAG = "ssdp";

static void maybe_update_manager_url(const char *message)
{
    const char *header = strstr(message, "X-Esp-Vp-Receiver-Url:");
    if (header == NULL) {
        return;
    }
    header += strlen("X-Esp-Vp-Receiver-Url:");
    while (*header == ' ' || *header == '\t') {
        header++;
    }
    const char *end = strpbrk(header, "\r\n");
    if (end == NULL || end == header) {
        return;
    }
    char url[128];
    size_t len = (size_t)(end - header);
    if (len >= sizeof(url)) {
        len = sizeof(url) - 1;
    }
    memcpy(url, header, len);
    url[len] = '\0';
    esp_vp_set_upload_base_url(url);
}

static int build_ssdp_response(char *out, size_t out_len, const char *prefix)
{
    const char *ip = wifi_local_ip();
    char bambu_headers[512] = "";
    if (esp_vp_is_configured()) {
        snprintf(bambu_headers, sizeof(bambu_headers),
            "DevModel.bambu.com: %s\r\n"
            "DevName.bambu.com: %s\r\n"
            "DevSignal.bambu.com: -44\r\n"
            "DevConnect.bambu.com: lan\r\n"
            "DevBind.bambu.com: free\r\n"
            "Devseclink.bambu.com: secure\r\n"
            "DevInf.bambu.com: wlan0\r\n"
            "DevVersion.bambu.com: 01.07.00.00\r\n"
            "DevCap.bambu.com: 1\r\n",
            esp_vp_model_code(), esp_vp_name());
    }
    return snprintf(out, out_len,
        "%s\r\n"
        "Server: UPnP/1.0\r\n"
        "Location: %s\r\n"
        "ST: urn:bambulab-com:device:3dprinter:1\r\n"
        "USN: %s\r\n"
        "Cache-Control: max-age=1800\r\n"
        "%s"
        "X-Esp-Vp-Device-Id: %s\r\n"
        "X-Esp-Vp-Firmware: %s\r\n"
        "X-Esp-Vp-Name: %s\r\n"
        "X-Esp-Vp-Configured: %s\r\n"
        "X-Esp-Vp-Paired: %s\r\n"
        "X-Esp-Vp-Pair-Ready: %s\r\n"
        "X-Esp-Vp-Pair-Remaining-Seconds: %d\r\n"
        "X-Esp-Vp-Managed: true\r\n"
        "X-Esp-Vp-Management-Url: http://%s:%d\r\n"
        "X-Esp-Vp-Upload-Base-Url: %s\r\n\r\n",
        prefix, ip, esp_vp_serial(), bambu_headers, esp_vp_device_id(),
        esp_vp_firmware_version(), esp_vp_name(), esp_vp_is_configured() ? "true" : "false",
        esp_vp_is_paired() ? "true" : "false", esp_vp_pair_ready() ? "true" : "false",
        esp_vp_pair_remaining_seconds(),
        ip, ESP_VP_MANAGEMENT_PORT, esp_vp_upload_base_url());
}

static int build_ssdp_notify(char *out, size_t out_len)
{
    const char *ip = wifi_local_ip();
    char bambu_headers[512] = "";
    if (esp_vp_is_configured()) {
        snprintf(bambu_headers, sizeof(bambu_headers),
            "DevModel.bambu.com: %s\r\n"
            "DevName.bambu.com: %s\r\n"
            "DevSignal.bambu.com: -44\r\n"
            "DevConnect.bambu.com: lan\r\n"
            "DevBind.bambu.com: free\r\n"
            "Devseclink.bambu.com: secure\r\n"
            "DevInf.bambu.com: wlan0\r\n"
            "DevVersion.bambu.com: 01.07.00.00\r\n"
            "DevCap.bambu.com: 1\r\n",
            esp_vp_model_code(), esp_vp_name());
    }
    return snprintf(out, out_len,
        "NOTIFY * HTTP/1.1\r\n"
        "Host: 239.255.255.250:1990\r\n"
        "Server: UPnP/1.0\r\n"
        "Location: %s\r\n"
        "NT: urn:bambulab-com:device:3dprinter:1\r\n"
        "NTS: ssdp:alive\r\n"
        "USN: %s\r\n"
        "Cache-Control: max-age=1800\r\n"
        "%s"
        "X-Esp-Vp-Device-Id: %s\r\n"
        "X-Esp-Vp-Firmware: %s\r\n"
        "X-Esp-Vp-Name: %s\r\n"
        "X-Esp-Vp-Configured: %s\r\n"
        "X-Esp-Vp-Paired: %s\r\n"
        "X-Esp-Vp-Pair-Ready: %s\r\n"
        "X-Esp-Vp-Pair-Remaining-Seconds: %d\r\n"
        "X-Esp-Vp-Managed: true\r\n"
        "X-Esp-Vp-Management-Url: http://%s:%d\r\n"
        "X-Esp-Vp-Upload-Base-Url: %s\r\n\r\n",
        ip, esp_vp_serial(), bambu_headers, esp_vp_device_id(),
        esp_vp_firmware_version(), esp_vp_name(), esp_vp_is_configured() ? "true" : "false",
        esp_vp_is_paired() ? "true" : "false", esp_vp_pair_ready() ? "true" : "false",
        esp_vp_pair_remaining_seconds(),
        ip, ESP_VP_MANAGEMENT_PORT, esp_vp_upload_base_url());
}

static void send_notify(int sock, char *tx, size_t tx_len)
{
    int n = build_ssdp_notify(tx, tx_len);
    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(ESP_VP_SSDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    sendto(sock, tx, n, 0, (struct sockaddr *)&dest, sizeof(dest));
    ESP_LOGI(TAG, "notify broadcast sent location=%s", wifi_local_ip());
}

static void ssdp_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(ESP_VP_SSDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind failed: errno=%d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    char rx[512];
    char tx[1024];
    ESP_LOGI(TAG, "listening UDP/%d", ESP_VP_SSDP_PORT);
    send_notify(sock, tx, sizeof(tx));
    TickType_t last_notify = xTaskGetTickCount();
    while (true) {
        struct sockaddr_in source = {0};
        socklen_t slen = sizeof(source);
        struct timeval tv = {
            .tv_sec = 1,
            .tv_usec = 0,
        };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int len = recvfrom(sock, rx, sizeof(rx) - 1, 0, (struct sockaddr *)&source, &slen);
        if (len >= 0) {
            rx[len] = '\0';
            if (strstr(rx, "M-SEARCH") &&
                (strstr(rx, "urn:bambulab-com:device:3dprinter:1") || strstr(rx, "ssdp:all"))) {
                maybe_update_manager_url(rx);
                int n = build_ssdp_response(tx, sizeof(tx), "HTTP/1.1 200 OK");
                sendto(sock, tx, n, 0, (struct sockaddr *)&source, slen);
                status_led_pulse(ESP_VP_STATUS_CLIENT_ACTIVE, 700);
                ESP_LOGI(TAG, "discovery response sent to %s:%d location=%s",
                    inet_ntoa(source.sin_addr),
                    ntohs(source.sin_port),
                    wifi_local_ip());
            }
        }

        if (xTaskGetTickCount() - last_notify > pdMS_TO_TICKS(30000)) {
            send_notify(sock, tx, sizeof(tx));
            last_notify = xTaskGetTickCount();
        }
    }
}

void ssdp_start(void)
{
    xTaskCreate(ssdp_task, "ssdp", 4096, NULL, 5, NULL);
}
