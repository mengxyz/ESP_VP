#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "app_config.h"
#include "stream_upload.h"

static const char *TAG = "stream_upload";

typedef struct {
    char host[96];
    char port[8];
    char path[128];
} upload_url_t;

static esp_err_t parse_upload_url(upload_url_t *out)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->port, "80", sizeof(out->port));

    const char *url = APP_BAMBUDDY_BASE_URL;
    const char *cursor = strstr(url, "://");
    if (cursor != NULL) {
        if (strncmp(url, "http://", 7) != 0) {
            ESP_LOGE(TAG, "only http:// Bambuddy URLs are supported by ESP streaming");
            return ESP_ERR_NOT_SUPPORTED;
        }
        cursor += 3;
    } else {
        cursor = url;
    }

    const char *path = strchr(cursor, '/');
    const char *host_end = path != NULL ? path : cursor + strlen(cursor);
    const char *port = memchr(cursor, ':', (size_t)(host_end - cursor));
    const char *host_stop = port != NULL ? port : host_end;

    size_t host_len = (size_t)(host_stop - cursor);
    if (host_len == 0 || host_len >= sizeof(out->host)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(out->host, cursor, host_len);
    out->host[host_len] = '\0';

    if (port != NULL) {
        size_t port_len = (size_t)(host_end - port - 1);
        if (port_len == 0 || port_len >= sizeof(out->port)) {
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(out->port, port + 1, port_len);
        out->port[port_len] = '\0';
    }

    if (path != NULL && strcmp(path, "/") != 0) {
        size_t base_len = strlen(path);
        while (base_len > 0 && path[base_len - 1] == '/') {
            base_len--;
        }
        if (base_len + strlen("/api/v1/esp-vp/upload") >= sizeof(out->path)) {
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(out->path, path, base_len);
        out->path[base_len] = '\0';
        strlcat(out->path, "/api/v1/esp-vp/upload", sizeof(out->path));
    } else {
        strlcpy(out->path, "/api/v1/esp-vp/upload", sizeof(out->path));
    }
    return ESP_OK;
}

static esp_err_t send_all(int sock, const void *data, size_t len)
{
    const char *cursor = (const char *)data;
    while (len > 0) {
        int written = send(sock, cursor, len, 0);
        if (written <= 0) {
            ESP_LOGE(TAG, "socket write failed errno=%d", errno);
            return ESP_FAIL;
        }
        cursor += written;
        len -= (size_t)written;
    }
    return ESP_OK;
}

static esp_err_t append_header(char *headers, size_t headers_len, int *offset,
                               const char *fmt, const char *value)
{
    if (*offset < 0 || (size_t)*offset >= headers_len) {
        return ESP_ERR_NO_MEM;
    }
    int written = snprintf(headers + *offset, headers_len - (size_t)*offset, fmt, value);
    if (written < 0 || (size_t)written >= headers_len - (size_t)*offset) {
        return ESP_ERR_NO_MEM;
    }
    *offset += written;
    return ESP_OK;
}

static esp_err_t finish_headers(char *headers, size_t headers_len, int *offset)
{
    if (*offset < 0 || (size_t)*offset + 2 >= headers_len) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(headers + *offset, "\r\n", 3);
    *offset += 2;
    return ESP_OK;
}

static int connect_socket(const upload_url_t *url)
{
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    int err = getaddrinfo(url->host, url->port, &hints, &res);
    if (err != 0 || res == NULL) {
        ESP_LOGE(TAG, "resolve failed host=%s port=%s err=%d", url->host, url->port, err);
        return -1;
    }

    int sock = -1;
    for (struct addrinfo *it = res; it != NULL; it = it->ai_next) {
        sock = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sock < 0) {
            continue;
        }
        struct timeval timeout = {
            .tv_sec = 30,
            .tv_usec = 0,
        };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (connect(sock, it->ai_addr, it->ai_addrlen) == 0) {
            break;
        }
        ESP_LOGE(TAG, "connect failed errno=%d", errno);
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    return sock;
}

esp_err_t stream_upload_begin(stream_upload_t *upload, const char *filename, const char *source_ip)
{
    memset(upload, 0, sizeof(*upload));
    upload->sock = -1;
    strlcpy(upload->filename, filename ? filename : "upload.3mf", sizeof(upload->filename));
    strlcpy(upload->source_ip, source_ip ? source_ip : "", sizeof(upload->source_ip));

    upload_url_t url;
    esp_err_t err = parse_upload_url(&url);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "opening upload http://%s:%s%s filename=%s", url.host, url.port, url.path, upload->filename);
    int sock = connect_socket(&url);
    if (sock < 0) {
        return ESP_FAIL;
    }

    char headers[512];
    int len = snprintf(headers, sizeof(headers),
                       "POST %s HTTP/1.1\r\n"
                       "Host: %s:%s\r\n"
                       "User-Agent: bambuddy-esp-vp/1\r\n"
                       "Content-Type: application/octet-stream\r\n"
                       "Transfer-Encoding: chunked\r\n"
                       "Connection: close\r\n"
                       "X-Bambuddy-Filename: %s\r\n"
                       "X-Bambuddy-VP-Name: %s\r\n",
                       url.path, url.host, url.port, upload->filename, APP_VP_NAME);
    if (len < 0 || (size_t)len >= sizeof(headers)) {
        close(sock);
        return ESP_ERR_NO_MEM;
    }
    err = ESP_OK;
    if (upload->source_ip[0] != '\0') {
        err = append_header(headers, sizeof(headers), &len,
                            "X-Bambuddy-Source-IP: %s\r\n", upload->source_ip);
    }
    if (err == ESP_OK && strlen(APP_BAMBUDDY_API_KEY) > 0) {
        err = append_header(headers, sizeof(headers), &len,
                            "X-API-Key: %s\r\n", APP_BAMBUDDY_API_KEY);
    }
    if (err == ESP_OK) {
        err = finish_headers(headers, sizeof(headers), &len);
    }
    if (err != ESP_OK) {
        close(sock);
        return err;
    }

    err = send_all(sock, headers, (size_t)len);
    if (err != ESP_OK) {
        close(sock);
        return err;
    }

    upload->sock = sock;
    return ESP_OK;
}

esp_err_t stream_upload_write(stream_upload_t *upload, const unsigned char *data, size_t len)
{
    if (upload->sock < 0 || len == 0) {
        return ESP_OK;
    }

    char chunk_header[16];
    int header_len = snprintf(chunk_header, sizeof(chunk_header), "%x\r\n", (unsigned)len);
    if (header_len < 0 || header_len >= (int)sizeof(chunk_header)) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = send_all(upload->sock, chunk_header, (size_t)header_len);
    if (err == ESP_OK) {
        err = send_all(upload->sock, data, len);
    }
    if (err == ESP_OK) {
        err = send_all(upload->sock, "\r\n", 2);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write failed: requested=%u", (unsigned)len);
        return err;
    }
    upload->bytes += len;
    return ESP_OK;
}

esp_err_t stream_upload_finish(stream_upload_t *upload)
{
    if (upload->sock < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = send_all(upload->sock, "0\r\n\r\n", 5);
    if (err != ESP_OK) {
        close(upload->sock);
        upload->sock = -1;
        return err;
    }

    int got = recv(upload->sock, upload->response_buf, sizeof(upload->response_buf) - 1, 0);
    int status = 0;
    if (got > 0) {
        upload->response_buf[got] = '\0';
        sscanf(upload->response_buf, "HTTP/%*s %d", &status);
    }
    ESP_LOGI(TAG, "uploaded %u bytes, status=%d response_len=%d",
             (unsigned)upload->bytes, status, got);

    close(upload->sock);
    upload->sock = -1;
    return (status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
}

void stream_upload_abort(stream_upload_t *upload)
{
    if (upload->sock >= 0) {
        close(upload->sock);
        upload->sock = -1;
    }
}
