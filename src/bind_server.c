#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_vp.h"
#include "tls_socket.h"

static const char *TAG = "bind";

static int make_listener(int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        return -1;
    }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(sock, 2) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

static void write_detect_frame(tls_socket_t *client)
{
    char json[512];
    int json_len = snprintf(json, sizeof(json),
        "{\"login\":{\"bind\":\"free\",\"command\":\"detect\",\"connect\":\"lan\","
        "\"dev_cap\":1,\"id\":\"%s\",\"model\":\"%s\",\"name\":\"%s\","
        "\"sequence_id\":\"20000\",\"version\":\"01.07.00.00\"}}",
        esp_vp_serial(), esp_vp_model_code(), esp_vp_name());

    unsigned char frame[576];
    uint16_t total = (uint16_t)(json_len + 6);
    frame[0] = 0xa5;
    frame[1] = 0xa5;
    frame[2] = (unsigned char)(total & 0xff);
    frame[3] = (unsigned char)((total >> 8) & 0xff);
    memcpy(&frame[4], json, json_len);
    frame[4 + json_len] = 0xa7;
    frame[5 + json_len] = 0xa7;
    tls_socket_write(client, frame, json_len + 6);
}

static void bind_task(void *arg)
{
    int port = (int)(intptr_t)arg;
    bool use_tls = port == ESP_VP_BIND_PORT_TLS;
    int listener = make_listener(port);
    if (listener < 0) {
        ESP_LOGW(TAG, "port %d unavailable: errno=%d", port, errno);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "listening TCP/%d", port);

    while (true) {
        struct sockaddr_in peer = {0};
        socklen_t peer_len = sizeof(peer);
        int client = accept(listener, (struct sockaddr *)&peer, &peer_len);
        if (client < 0) {
            continue;
        }
        ESP_LOGI(TAG, "accepted TCP/%d from %s", port, inet_ntoa(peer.sin_addr));
        status_led_pulse(ESP_VP_STATUS_CLIENT_ACTIVE, 900);
        tls_socket_t *sock = calloc(1, sizeof(*sock));
        if (sock == NULL) {
            ESP_LOGW(TAG, "no memory for TCP/%d client", port);
            close(client);
            continue;
        }
        if (tls_socket_init(sock, client, use_tls) != ESP_OK) {
            ESP_LOGW(TAG, "session init failed on TCP/%d", port);
            close(client);
            free(sock);
            continue;
        }
        unsigned char scratch[256];
        tls_socket_read(sock, scratch, sizeof(scratch));
        write_detect_frame(sock);
        ESP_LOGI(TAG, "detect frame sent on TCP/%d", port);
        tls_socket_close(sock);
        free(sock);
    }
}

void bind_server_start(void)
{
    xTaskCreate(bind_task, "bind3000", 4096, (void *)(intptr_t)ESP_VP_BIND_PORT_PLAIN, 5, NULL);
    xTaskCreate(bind_task, "bind3002", 8192, (void *)(intptr_t)ESP_VP_BIND_PORT_TLS, 5, NULL);
}
