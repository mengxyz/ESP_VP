#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_vp.h"

static const char *TAG = "management";

static bool has_valid_auth(const char *request)
{
    const char *stored = esp_vp_receiver_token();
    if (stored[0] == '\0') {
        return false;
    }
    const char *auth = strstr(request, "Authorization: Bearer ");
    if (auth == NULL) {
        return false;
    }
    auth += strlen("Authorization: Bearer ");
    size_t token_len = strlen(stored);
    if (strncmp(auth, stored, token_len) == 0 && (auth[token_len] == '\r' || auth[token_len] == '\n')) {
        return true;
    }
    return false;
}

static void send_response(int client, int status, const char *reason, const char *body)
{
    char headers[256];
    int body_len = body ? strlen(body) : 0;
    int len = snprintf(headers, sizeof(headers),
                       "HTTP/1.1 %d %s\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: %d\r\n"
                       "Connection: close\r\n\r\n",
                       status, reason, body_len);
    send(client, headers, len, 0);
    if (body_len > 0) {
        send(client, body, body_len, 0);
    }
}

static void handle_info(int client)
{
    char body[800];
    int len = snprintf(body, sizeof(body),
                       "{\"status\":\"ok\",\"firmware\":\"%s\",\"manager_mode\":%s,"
                       "\"configured\":%s,\"paired\":%s,\"pair_ready\":%s,\"pair_remaining_seconds\":%d,"
                       "\"device_id\":\"%s\","
                       "\"name\":\"%s\",\"model_code\":\"%s\",\"product_name\":\"%s\","
                       "\"serial\":\"%s\",\"access_code\":\"%s\",\"upload_base_url\":\"%s\","
                       "\"ip\":\"%s\"}",
                       esp_vp_firmware_version(),
                       esp_vp_manager_mode() ? "true" : "false",
                       esp_vp_is_configured() ? "true" : "false",
                       esp_vp_is_paired() ? "true" : "false",
                       esp_vp_pair_ready() ? "true" : "false",
                       esp_vp_pair_remaining_seconds(),
                       esp_vp_device_id(),
                       esp_vp_name(),
                       esp_vp_model_code(),
                       esp_vp_product_name(),
                       esp_vp_serial(),
                       esp_vp_access_code(),
                       esp_vp_upload_base_url(),
                       wifi_local_ip());
    if (len < 0 || len >= (int)sizeof(body)) {
        send_response(client, 500, "Internal Server Error", "{\"detail\":\"info response too large\"}");
        return;
    }
    send_response(client, 200, "OK", body);
}

static void handle_pair(int client, const char *request)
{
    if (!esp_vp_pair_ready()) {
        send_response(client, 409, "Conflict", "{\"detail\":\"pair mode is not active\"}");
        return;
    }
    const char *body = strstr(request, "\r\n\r\n");
    if (body == NULL) {
        send_response(client, 400, "Bad Request", "{\"detail\":\"missing JSON body\"}");
        return;
    }
    body += 4;
    esp_err_t err = esp_vp_pair_json(body);
    if (err != ESP_OK) {
        send_response(client, 400, "Bad Request", "{\"detail\":\"invalid pair payload\"}");
        return;
    }
    esp_vp_pair_mode_stop();
    status_led_pulse(ESP_VP_STATUS_CLIENT_ACTIVE, 1400);
    send_response(client, 200, "OK", "{\"status\":\"paired\",\"paired\":true}");
}

static void handle_config(int client, const char *request)
{
    if (!has_valid_auth(request)) {
        send_response(client, 401, "Unauthorized", "{\"detail\":\"invalid token\"}");
        return;
    }
    const char *body = strstr(request, "\r\n\r\n");
    if (body == NULL) {
        send_response(client, 400, "Bad Request", "{\"detail\":\"missing JSON body\"}");
        return;
    }
    body += 4;
    esp_err_t err = esp_vp_apply_config_json(body);
    if (err != ESP_OK) {
        send_response(client, 400, "Bad Request", "{\"detail\":\"invalid config\"}");
        return;
    }
    status_led_pulse(ESP_VP_STATUS_CLIENT_ACTIVE, 900);
    send_response(client, 200, "OK", "{\"status\":\"pushed\",\"configured\":true}");
}

static int content_length_from_request(const char *request)
{
    const char *cl = strstr(request, "Content-Length:");
    if (cl == NULL) {
        cl = strstr(request, "content-length:");
    }
    if (cl == NULL) {
        return 0;
    }
    return atoi(cl + strlen("Content-Length:"));
}

static void handle_ota(int client, const char *request, int total, int header_len, int content_len)
{
    if (!has_valid_auth(request)) {
        send_response(client, 401, "Unauthorized", "{\"detail\":\"invalid token\"}");
        return;
    }
    if (content_len <= 0) {
        send_response(client, 400, "Bad Request", "{\"detail\":\"missing firmware body\"}");
        return;
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL) {
        send_response(client, 409, "Conflict", "{\"detail\":\"OTA partition is not available; flash OTA partition layout over USB first\"}");
        return;
    }
    if ((uint32_t)content_len > partition->size) {
        send_response(client, 413, "Payload Too Large", "{\"detail\":\"firmware image is larger than OTA partition\"}");
        return;
    }

    ESP_LOGI(TAG, "OTA begin partition=%s offset=0x%lx size=%lu content_len=%d",
             partition->label,
             (unsigned long)partition->address,
             (unsigned long)partition->size,
             content_len);

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(partition, content_len, &ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        send_response(client, 500, "Internal Server Error", "{\"detail\":\"OTA begin failed\"}");
        return;
    }

    int written = 0;
    int body_in_buffer = total - header_len;
    if (body_in_buffer > 0) {
        if (body_in_buffer > content_len) {
            body_in_buffer = content_len;
        }
        err = esp_ota_write(ota, request + header_len, body_in_buffer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA buffered write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota);
            send_response(client, 500, "Internal Server Error", "{\"detail\":\"OTA write failed\"}");
            return;
        }
        written += body_in_buffer;
    }

    char chunk[4096];
    while (written < content_len) {
        int want = content_len - written;
        if (want > (int)sizeof(chunk)) {
            want = sizeof(chunk);
        }
        int got = recv(client, chunk, want, 0);
        if (got <= 0) {
            ESP_LOGE(TAG, "OTA body read failed after %d/%d bytes", written, content_len);
            esp_ota_abort(ota);
            send_response(client, 400, "Bad Request", "{\"detail\":\"firmware body ended early\"}");
            return;
        }
        err = esp_ota_write(ota, chunk, got);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed after %d bytes: %s", written, esp_err_to_name(err));
            esp_ota_abort(ota);
            send_response(client, 500, "Internal Server Error", "{\"detail\":\"OTA write failed\"}");
            return;
        }
        written += got;
    }

    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
        send_response(client, 400, "Bad Request", "{\"detail\":\"invalid firmware image\"}");
        return;
    }
    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA set boot partition failed: %s", esp_err_to_name(err));
        send_response(client, 500, "Internal Server Error", "{\"detail\":\"failed to set OTA boot partition\"}");
        return;
    }

    ESP_LOGI(TAG, "OTA complete bytes=%d next=%s; rebooting", written, partition->label);
    send_response(client, 200, "OK", "{\"status\":\"ota_applied\",\"rebooting\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void management_client(int client)
{
    char request[12288];
    int total = 0;
    while (total < (int)sizeof(request) - 1) {
        int got = recv(client, request + total, sizeof(request) - 1 - total, 0);
        if (got <= 0) {
            break;
        }
        total += got;
        request[total] = '\0';
        char *header_end = strstr(request, "\r\n\r\n");
        if (header_end != NULL) {
            if (strncmp(request, "POST /api/v1/device/ota ", 24) == 0) {
                break;
            }
            int content_len = content_length_from_request(request);
            int header_len = (int)(header_end + 4 - request);
            if (total >= header_len + content_len) {
                break;
            }
        }
    }
    request[total] = '\0';

    if (strncmp(request, "GET /api/v1/device/info ", 24) == 0) {
        handle_info(client);
    } else if (strncmp(request, "POST /api/v1/device/pair ", 25) == 0) {
        handle_pair(client, request);
    } else if (strncmp(request, "POST /api/v1/device/config ", 27) == 0) {
        handle_config(client, request);
    } else if (strncmp(request, "POST /api/v1/device/ota ", 24) == 0) {
        char *header_end = strstr(request, "\r\n\r\n");
        if (header_end == NULL) {
            send_response(client, 400, "Bad Request", "{\"detail\":\"missing headers\"}");
        } else {
            int header_len = (int)(header_end + 4 - request);
            handle_ota(client, request, total, header_len, content_length_from_request(request));
        }
    } else {
        send_response(client, 404, "Not Found", "{\"detail\":\"not found\"}");
    }
    close(client);
}

static void management_task(void *arg)
{
    (void)arg;
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listener < 0) {
        ESP_LOGE(TAG, "socket failed errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(ESP_VP_MANAGEMENT_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(listener, 2) < 0) {
        ESP_LOGE(TAG, "listen failed errno=%d", errno);
        close(listener);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "listening TCP/%d", ESP_VP_MANAGEMENT_PORT);
    while (true) {
        struct sockaddr_in peer = {0};
        socklen_t peer_len = sizeof(peer);
        int client = accept(listener, (struct sockaddr *)&peer, &peer_len);
        if (client < 0) {
            continue;
        }
        ESP_LOGI(TAG, "accepted TCP/%d from %s", ESP_VP_MANAGEMENT_PORT, inet_ntoa(peer.sin_addr));
        management_client(client);
    }
}

void management_server_start(void)
{
    xTaskCreate(management_task, "management", 32768, NULL, 5, NULL);
}
