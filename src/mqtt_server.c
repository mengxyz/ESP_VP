#include <errno.h>
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

static const char *TAG = "mqtt";
static volatile int s_active_mqtt_clients = 0;

static int read_remaining_length(tls_socket_t *client)
{
    int multiplier = 1;
    int value = 0;
    unsigned char encoded;
    do {
        if (tls_socket_read(client, &encoded, 1) != 1) {
            return -1;
        }
        value += (encoded & 127) * multiplier;
        multiplier *= 128;
    } while ((encoded & 128) != 0);
    return value;
}

static void send_connack(tls_socket_t *client, unsigned char code)
{
    unsigned char pkt[] = {0x20, 0x02, 0x00, code};
    tls_socket_write(client, pkt, sizeof(pkt));
}

static int write_remaining_length(unsigned char *out, int value)
{
    int idx = 0;
    do {
        unsigned char encoded = value % 128;
        value /= 128;
        if (value > 0) {
            encoded |= 128;
        }
        out[idx++] = encoded;
    } while (value > 0 && idx < 4);
    return idx;
}

static void send_publish(tls_socket_t *client, const char *topic, const char *payload)
{
    uint16_t topic_len = strlen(topic);
    int payload_len = strlen(payload);
    int remaining = 2 + topic_len + payload_len;
    unsigned char hdr[5] = {0x30};
    int idx = 1 + write_remaining_length(&hdr[1], remaining);
    tls_socket_write(client, hdr, idx);
    unsigned char tlen[] = {(unsigned char)(topic_len >> 8), (unsigned char)(topic_len & 0xff)};
    tls_socket_write(client, tlen, 2);
    tls_socket_write(client, topic, topic_len);
    tls_socket_write(client, payload, payload_len);
}

static void send_publish_status(tls_socket_t *client, const char *serial)
{
    char topic[96];
    char payload[1536];
    snprintf(topic, sizeof(topic), "device/%s/report", serial);
    const char *proxy_report = proxy_status_report_json();
    if (strcmp(esp_vp_mode(), "proxy_status") == 0 && proxy_report != NULL) {
        send_publish(client, topic, proxy_report);
        return;
    }
    snprintf(payload, sizeof(payload),
        "{\n"
        "    \"print\": {\n"
        "        \"sequence_id\": \"1\",\n"
        "        \"command\": \"push_status\",\n"
        "        \"msg\": 0,\n"
        "        \"gcode_state\": \"IDLE\",\n"
        "        \"gcode_file\": \"\",\n"
        "        \"gcode_file_prepare_percent\": \"0\",\n"
        "        \"subtask_name\": \"\",\n"
        "        \"mc_print_stage\": \"\",\n"
        "        \"mc_percent\": 0,\n"
        "        \"mc_remaining_time\": 0,\n"
        "        \"wifi_signal\": \"-44dBm\",\n"
        "        \"home_flag\": 256,\n"
        "        \"sdcard\": true,\n"
        "        \"storage\": {\"free\": 1000000000, \"total\": 32000000000},\n"
        "        \"online\": {\"ahb\": false, \"rfid\": false, \"version\": 7},\n"
        "        \"ams_status\": 0,\n"
        "        \"nozzle_diameter\": \"0.4\",\n"
        "        \"nozzle_type\": \"hardened_steel\"\n"
        "    }\n"
        "}");
    send_publish(client, topic, payload);
}

static void send_version_response(tls_socket_t *client, const char *serial, const char *sequence_id)
{
    char topic[96];
    char payload[1792];
    snprintf(topic, sizeof(topic), "device/%s/report", serial);
    snprintf(payload, sizeof(payload),
        "{\n"
        "    \"info\": {\n"
        "        \"command\": \"get_version\",\n"
        "        \"sequence_id\": \"%s\",\n"
        "        \"module\": [\n"
        "            {\"name\":\"ota\",\"product_name\":\"%s\",\"sw_ver\":\"01.07.00.00\",\"sw_new_ver\":\"\",\"hw_ver\":\"OTA\",\"sn\":\"%s\",\"flag\":0},\n"
        "            {\"name\":\"esp32\",\"product_name\":\"%s\",\"sw_ver\":\"01.07.22.25\",\"sw_new_ver\":\"\",\"hw_ver\":\"AP05\",\"sn\":\"%s\",\"flag\":0},\n"
        "            {\"name\":\"rv1126\",\"product_name\":\"%s\",\"sw_ver\":\"00.00.27.38\",\"sw_new_ver\":\"\",\"hw_ver\":\"AP05\",\"sn\":\"%s\",\"flag\":0},\n"
        "            {\"name\":\"th\",\"product_name\":\"%s\",\"sw_ver\":\"00.00.04.00\",\"sw_new_ver\":\"\",\"hw_ver\":\"TH07\",\"sn\":\"%s\",\"flag\":0},\n"
        "            {\"name\":\"mc\",\"product_name\":\"%s\",\"sw_ver\":\"00.00.10.00\",\"sw_new_ver\":\"\",\"hw_ver\":\"MC07\",\"sn\":\"%s\",\"flag\":0}\n"
        "        ]\n"
        "    }\n"
        "}",
        sequence_id,
        esp_vp_product_name(), serial,
        esp_vp_product_name(), serial,
        esp_vp_product_name(), serial,
        esp_vp_product_name(), serial,
        esp_vp_product_name(), serial);
    send_publish(client, topic, payload);
}

static bool extract_topic_serial(const char *topic, char *serial, size_t serial_len)
{
    const char *prefix = "device/";
    if (strncmp(topic, prefix, strlen(prefix)) != 0) {
        return false;
    }
    const char *start = topic + strlen(prefix);
    const char *slash = strchr(start, '/');
    if (slash == NULL || slash == start) {
        return false;
    }
    size_t len = slash - start;
    if (len >= serial_len) {
        len = serial_len - 1;
    }
    memcpy(serial, start, len);
    serial[len] = '\0';
    return true;
}

static void handle_subscribe(tls_socket_t *client, const unsigned char *buf, int len, char *effective_serial, size_t serial_len)
{
    if (len < 5) {
        return;
    }
    uint16_t packet_id = ((uint16_t)buf[0] << 8) | buf[1];
    int idx = 2;
    unsigned char granted[4];
    int granted_count = 0;
    while (idx + 3 <= len && granted_count < (int)sizeof(granted)) {
        uint16_t topic_len = ((uint16_t)buf[idx] << 8) | buf[idx + 1];
        idx += 2;
        if (idx + topic_len + 1 > len) {
            break;
        }
        char topic[128];
        int copy_len = topic_len < sizeof(topic) - 1 ? topic_len : sizeof(topic) - 1;
        memcpy(topic, &buf[idx], copy_len);
        topic[copy_len] = '\0';
        extract_topic_serial(topic, effective_serial, serial_len);
        idx += topic_len;
        unsigned char qos = buf[idx++];
        granted[granted_count++] = qos > 1 ? 1 : qos;
        ESP_LOGI(TAG, "subscribe: %s qos=%u", topic, qos);
    }

    unsigned char suback[8] = {0x90, (unsigned char)(2 + granted_count), (unsigned char)(packet_id >> 8), (unsigned char)(packet_id & 0xff)};
    memcpy(&suback[4], granted, granted_count);
    tls_socket_write(client, suback, 4 + granted_count);
    send_publish_status(client, effective_serial);
}

static void extract_sequence_id(const char *message, char *out, size_t out_len)
{
    strlcpy(out, "1", out_len);
    const char *key = strstr(message, "\"sequence_id\"");
    if (key == NULL) {
        return;
    }
    const char *colon = strchr(key, ':');
    if (colon == NULL) {
        return;
    }
    const char *quote = strchr(colon, '"');
    if (quote == NULL) {
        return;
    }
    quote++;
    const char *end = strchr(quote, '"');
    if (end == NULL) {
        return;
    }
    size_t len = end - quote;
    if (len >= out_len) {
        len = out_len - 1;
    }
    memcpy(out, quote, len);
    out[len] = '\0';
}

static void handle_publish(tls_socket_t *client, unsigned char header, const unsigned char *buf, int len, char *effective_serial, size_t serial_len)
{
    if (len < 3) {
        return;
    }
    int idx = 0;
    uint16_t topic_len = ((uint16_t)buf[idx] << 8) | buf[idx + 1];
    idx += 2;
    if (idx + topic_len > len) {
        return;
    }
    char topic[128];
    int copy_len = topic_len < sizeof(topic) - 1 ? topic_len : sizeof(topic) - 1;
    memcpy(topic, &buf[idx], copy_len);
    topic[copy_len] = '\0';
    idx += topic_len;
    int qos = (header & 0x06) >> 1;
    if (qos > 0) {
        idx += 2;
    }
    if (idx > len) {
        return;
    }
    char message[512];
    int msg_len = (len - idx) < (int)sizeof(message) - 1 ? (len - idx) : (int)sizeof(message) - 1;
    memcpy(message, &buf[idx], msg_len);
    message[msg_len] = '\0';
    extract_topic_serial(topic, effective_serial, serial_len);
    ESP_LOGI(TAG, "publish to %s: %.80s", topic, message);

    if (strstr(message, "\"pushall\"")) {
        send_publish_status(client, effective_serial);
    }
    if (strstr(message, "\"get_version\"")) {
        char sequence_id[32];
        extract_sequence_id(message, sequence_id, sizeof(sequence_id));
        send_version_response(client, effective_serial, sequence_id);
    }
}

static void mqtt_client_task(void *arg)
{
    int client = (int)(intptr_t)arg;
    tls_socket_t *sock = calloc(1, sizeof(*sock));
    if (sock == NULL) {
        close(client);
        if (s_active_mqtt_clients > 0) {
            s_active_mqtt_clients--;
        }
        vTaskDelete(NULL);
        return;
    }
    if (tls_socket_init(sock, client, true) != ESP_OK) {
        ESP_LOGW(TAG, "mqtt TLS/session init failed");
        close(client);
        free(sock);
        if (s_active_mqtt_clients > 0) {
            s_active_mqtt_clients--;
        }
        vTaskDelete(NULL);
        return;
    }
    char effective_serial[32];
    strlcpy(effective_serial, esp_vp_serial(), sizeof(effective_serial));
    unsigned char type;
    int first = tls_socket_read(sock, &type, 1);
    if (first != 1) {
        ESP_LOGW(TAG, "mqtt no CONNECT byte read: ret=%d", first);
    } else if ((type & 0xf0) == 0x10) {
        int remaining = read_remaining_length(sock);
        unsigned char buf[512];
        if (remaining > 0 && remaining <= (int)sizeof(buf)) {
            int got = tls_socket_read(sock, buf, remaining);
            if (got != remaining) {
                ESP_LOGW(TAG, "mqtt CONNECT short read: got=%d expected=%d", got, remaining);
                tls_socket_close(sock);
                free(sock);
                if (s_active_mqtt_clients > 0) {
                    s_active_mqtt_clients--;
                }
                vTaskDelete(NULL);
                return;
            }
            const char *username = "";
            const char *password = "";
            char username_buf[32] = "";
            char password_buf[32] = "";
            int idx = 0;
            bool parsed = false;
            if (remaining >= 10) {
                int proto_len = ((int)buf[idx] << 8) | buf[idx + 1];
                idx += 2 + proto_len;
                if (idx + 4 <= remaining) {
                    idx += 1; /* protocol level */
                    unsigned char flags = buf[idx++];
                    idx += 2; /* keepalive */
                    if (idx + 2 <= remaining) {
                        int client_id_len = ((int)buf[idx] << 8) | buf[idx + 1];
                        idx += 2 + client_id_len;
                    }
                    if ((flags & 0x80) && idx + 2 <= remaining) {
                        int username_len = ((int)buf[idx] << 8) | buf[idx + 1];
                        idx += 2;
                        if (idx + username_len <= remaining) {
                            int n = username_len < (int)sizeof(username_buf) - 1 ? username_len : (int)sizeof(username_buf) - 1;
                            memcpy(username_buf, &buf[idx], n);
                            username_buf[n] = '\0';
                            idx += username_len;
                            username = username_buf;
                        }
                    }
                    if ((flags & 0x40) && idx + 2 <= remaining) {
                        int password_len = ((int)buf[idx] << 8) | buf[idx + 1];
                        idx += 2;
                        if (idx + password_len <= remaining) {
                            int n = password_len < (int)sizeof(password_buf) - 1 ? password_len : (int)sizeof(password_buf) - 1;
                            memcpy(password_buf, &buf[idx], n);
                            password_buf[n] = '\0';
                            password = password_buf;
                        }
                    }
                    parsed = true;
                }
            }
            ESP_LOGI(TAG, "connect parsed=%d username='%s' password_len=%u", parsed, username, (unsigned)strlen(password));
            if (parsed && strcmp(username, "bblp") == 0 && strcmp(password, esp_vp_access_code()) == 0) {
                send_connack(sock, 0);
                send_publish_status(sock, effective_serial);
                ESP_LOGI(TAG, "client authenticated");
            } else {
                send_connack(sock, 5);
                status_led_pulse(ESP_VP_STATUS_ERROR, 1200);
                ESP_LOGW(TAG, "client auth failed");
                tls_socket_close(sock);
                free(sock);
                if (s_active_mqtt_clients > 0) {
                    s_active_mqtt_clients--;
                }
                vTaskDelete(NULL);
                return;
            }
        } else {
            ESP_LOGW(TAG, "mqtt CONNECT remaining invalid: %d", remaining);
            send_connack(sock, 4);
        }
    } else {
        ESP_LOGW(TAG, "mqtt first packet is not CONNECT: type=0x%02x", type);
    }
    while (tls_socket_read(sock, &type, 1) > 0) {
        int remaining = read_remaining_length(sock);
        if (remaining <= 0 || remaining > 1024) {
            break;
        }
        unsigned char buf[1024];
        int got_total = 0;
        while (got_total < remaining) {
            int got = tls_socket_read(sock, &buf[got_total], remaining - got_total);
            if (got <= 0) {
                got_total = -1;
                break;
            }
            got_total += got;
        }
        if (got_total != remaining) {
            break;
        }
        unsigned char packet_type = (type & 0xf0) >> 4;
        if (packet_type == 8) {
            handle_subscribe(sock, buf, remaining, effective_serial, sizeof(effective_serial));
        } else if (packet_type == 3) {
            handle_publish(sock, type, buf, remaining, effective_serial, sizeof(effective_serial));
        } else if (packet_type == 12) {
            unsigned char pong[] = {0xd0, 0x00};
            tls_socket_write(sock, pong, sizeof(pong));
        } else if (packet_type == 14) {
            break;
        }
    }
    tls_socket_close(sock);
    free(sock);
    if (s_active_mqtt_clients > 0) {
        s_active_mqtt_clients--;
    }
    ESP_LOGI(TAG, "client disconnected active=%d", s_active_mqtt_clients);
    vTaskDelete(NULL);
}

static void mqtt_task(void *arg)
{
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(ESP_VP_MQTT_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(listener, 2) < 0) {
        ESP_LOGW(TAG, "port %d unavailable: errno=%d", ESP_VP_MQTT_PORT, errno);
        close(listener);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "listening TCP/%d", ESP_VP_MQTT_PORT);
    while (true) {
        struct sockaddr_in peer = {0};
        socklen_t peer_len = sizeof(peer);
        int client = accept(listener, (struct sockaddr *)&peer, &peer_len);
        if (client >= 0) {
            if (s_active_mqtt_clients >= 1) {
                ESP_LOGW(TAG, "rejecting extra MQTT client from %s active=%d", inet_ntoa(peer.sin_addr), s_active_mqtt_clients);
                close(client);
                continue;
            }
            s_active_mqtt_clients++;
            ESP_LOGI(TAG, "accepted TCP/%d from %s", ESP_VP_MQTT_PORT, inet_ntoa(peer.sin_addr));
            status_led_pulse(ESP_VP_STATUS_CLIENT_ACTIVE, 900);
            BaseType_t created = xTaskCreate(mqtt_client_task, "mqtt_client", 8192, (void *)(intptr_t)client, 5, NULL);
            if (created != pdPASS) {
                s_active_mqtt_clients--;
                close(client);
                ESP_LOGW(TAG, "failed to create MQTT client task");
            }
        }
    }
}

void mqtt_server_start(void)
{
    xTaskCreate(mqtt_task, "mqtt", 4096, NULL, 5, NULL);
}
