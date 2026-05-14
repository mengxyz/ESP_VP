#include <errno.h>
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

static int build_ssdp_response(char *out, size_t out_len, const char *prefix)
{
    const char *ip = wifi_local_ip();
    return snprintf(out, out_len,
        "%s\r\n"
        "Server: UPnP/1.0\r\n"
        "Location: %s\r\n"
        "ST: urn:bambulab-com:device:3dprinter:1\r\n"
        "USN: %s\r\n"
        "Cache-Control: max-age=1800\r\n"
        "DevModel.bambu.com: %s\r\n"
        "DevName.bambu.com: %s\r\n"
        "DevSignal.bambu.com: -44\r\n"
        "DevConnect.bambu.com: lan\r\n"
        "DevBind.bambu.com: free\r\n"
        "Devseclink.bambu.com: secure\r\n"
        "DevInf.bambu.com: wlan0\r\n"
        "DevVersion.bambu.com: 01.07.00.00\r\n"
        "DevCap.bambu.com: 1\r\n\r\n",
        prefix, ip, ESP_VP_SERIAL, APP_VP_MODEL_CODE, APP_VP_NAME);
}

static int build_ssdp_notify(char *out, size_t out_len)
{
    const char *ip = wifi_local_ip();
    return snprintf(out, out_len,
        "NOTIFY * HTTP/1.1\r\n"
        "Host: 239.255.255.250:1990\r\n"
        "Server: UPnP/1.0\r\n"
        "Location: %s\r\n"
        "NT: urn:bambulab-com:device:3dprinter:1\r\n"
        "NTS: ssdp:alive\r\n"
        "USN: %s\r\n"
        "Cache-Control: max-age=1800\r\n"
        "DevModel.bambu.com: %s\r\n"
        "DevName.bambu.com: %s\r\n"
        "DevSignal.bambu.com: -44\r\n"
        "DevConnect.bambu.com: lan\r\n"
        "DevBind.bambu.com: free\r\n"
        "Devseclink.bambu.com: secure\r\n"
        "DevInf.bambu.com: wlan0\r\n"
        "DevVersion.bambu.com: 01.07.00.00\r\n"
        "DevCap.bambu.com: 1\r\n\r\n",
        ip, ESP_VP_SERIAL, APP_VP_MODEL_CODE, APP_VP_NAME);
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
                int n = build_ssdp_response(tx, sizeof(tx), "HTTP/1.1 200 OK");
                sendto(sock, tx, n, 0, (struct sockaddr *)&source, slen);
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
