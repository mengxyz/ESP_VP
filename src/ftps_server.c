#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_vp.h"
#include "stream_upload.h"
#include "tls_socket.h"

static const char *TAG = "ftps";
static int s_next_passive_port = ESP_VP_FTPS_PASSIVE_PORT;

typedef struct {
    tls_socket_t control;
    char source_ip[48];
    int passive_listener;
    int passive_port;
} ftp_session_t;

static void ftp_send(tls_socket_t *sock, int code, const char *message)
{
    char line[192];
    int len = snprintf(line, sizeof(line), "%d %s\r\n", code, message);
    tls_socket_write(sock, line, len);
}

static int make_listener(int port, int backlog)
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
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(sock, backlog) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

static void set_nonblocking(int sock)
{
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }
}

static void close_passive(ftp_session_t *session)
{
    if (session->passive_listener >= 0) {
        close(session->passive_listener);
        session->passive_listener = -1;
        session->passive_port = 0;
    }
}

static int open_next_passive_listener(int *port)
{
    for (int attempt = 0; attempt < ESP_VP_FTPS_PASSIVE_PORT_COUNT; attempt++) {
        int candidate = s_next_passive_port;
        s_next_passive_port++;
        if (s_next_passive_port >= ESP_VP_FTPS_PASSIVE_PORT + ESP_VP_FTPS_PASSIVE_PORT_COUNT) {
            s_next_passive_port = ESP_VP_FTPS_PASSIVE_PORT;
        }

        int listener = make_listener(candidate, 1);
        if (listener >= 0) {
            *port = candidate;
            return listener;
        }
        ESP_LOGW(TAG, "passive port %d unavailable errno=%d", candidate, errno);
    }
    return -1;
}

static void handle_pasv(ftp_session_t *session)
{
    close_passive(session);
    int passive_port = 0;
    session->passive_listener = open_next_passive_listener(&passive_port);
    if (session->passive_listener < 0) {
        ftp_send(&session->control, 425, "Cannot open passive listener");
        return;
    }
    session->passive_port = passive_port;

    int ip1 = 0;
    int ip2 = 0;
    int ip3 = 0;
    int ip4 = 0;
    if (sscanf(wifi_local_ip(), "%d.%d.%d.%d", &ip1, &ip2, &ip3, &ip4) != 4) {
        ip1 = 0;
        ip2 = 0;
        ip3 = 0;
        ip4 = 0;
    }
    int p1 = session->passive_port / 256;
    int p2 = session->passive_port % 256;
    char msg[128];
    snprintf(msg, sizeof(msg), "Entering Passive Mode (%d,%d,%d,%d,%d,%d)", ip1, ip2, ip3, ip4, p1, p2);
    ftp_send(&session->control, 227, msg);
    ESP_LOGI(TAG, "PASV listening on %s:%d", wifi_local_ip(), session->passive_port);
}

static void handle_epsv(ftp_session_t *session)
{
    close_passive(session);
    ftp_send(&session->control, 502, "Use PASV");
    ESP_LOGI(TAG, "EPSV refused; forcing PASV with explicit LAN address");
}

static esp_err_t relay_stor_data(ftp_session_t *session, const char *filename)
{
    if (session->passive_listener < 0) {
        ftp_send(&session->control, 425, "Use PASV first");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "receiving file: %s from %s", filename, session->source_ip);
    ftp_send(&session->control, 150, "Opening data connection");
    ESP_LOGI(TAG, "waiting for passive data connection on TCP/%d", session->passive_port);

    set_nonblocking(session->passive_listener);
    int data_fd = -1;
    int passive_port = session->passive_port;
    int64_t deadline_us = esp_timer_get_time() + 20000000;
    while (esp_timer_get_time() < deadline_us) {
        struct sockaddr_in data_peer_wait = {0};
        socklen_t data_peer_wait_len = sizeof(data_peer_wait);
        data_fd = accept(session->passive_listener, (struct sockaddr *)&data_peer_wait, &data_peer_wait_len);
        if (data_fd >= 0) {
            break;
        }
        int saved_errno = errno;
        if (saved_errno != EWOULDBLOCK && saved_errno != EAGAIN) {
            close_passive(session);
            ESP_LOGW(TAG, "passive data connection accept failed errno=%d", saved_errno);
            ftp_send(&session->control, 425, "Data connection failed");
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    struct sockaddr_in data_peer = {0};
    socklen_t data_peer_len = sizeof(data_peer);
    if (data_fd < 0) {
        close_passive(session);
        ESP_LOGW(TAG, "passive data connection timed out on TCP/%d from control peer %s",
                 passive_port, session->source_ip);
        ftp_send(&session->control, 425, "Data connection timed out");
        return ESP_FAIL;
    }
    if (getpeername(data_fd, (struct sockaddr *)&data_peer, &data_peer_len) != 0) {
        memset(&data_peer, 0, sizeof(data_peer));
    }
    close_passive(session);
    tls_socket_t *data = calloc(1, sizeof(*data));
    if (data == NULL) {
        close(data_fd);
        ftp_send(&session->control, 425, "No memory");
        return ESP_ERR_NO_MEM;
    }
    if (tls_socket_init(data, data_fd, true) != ESP_OK) {
        close(data_fd);
        free(data);
        ftp_send(&session->control, 425, "TLS data connection failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "data TLS established from %s", inet_ntoa(data_peer.sin_addr));

    bool archive_upload = strstr(filename, ".3mf") != NULL;
    stream_upload_t upload;
    memset(&upload, 0, sizeof(upload));
    if (archive_upload) {
        status_led_set(ESP_VP_STATUS_UPLOADING);
        esp_err_t begin_err = stream_upload_begin(&upload, filename, session->source_ip);
        if (begin_err != ESP_OK) {
            status_led_set(ESP_VP_STATUS_READY);
            status_led_pulse(ESP_VP_STATUS_ERROR, 1600);
            tls_socket_close(data);
            free(data);
            ftp_send(&session->control, 451, "Backend upload failed");
            return begin_err;
        }
    }

    esp_err_t err = ESP_OK;

    size_t buffer_len = 64 * 1024;
    unsigned char *buffer = heap_caps_malloc(buffer_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer_len = 16 * 1024;
        buffer = malloc(buffer_len);
    }
    if (buffer == NULL) {
        stream_upload_abort(&upload);
        tls_socket_close(data);
        free(data);
        ftp_send(&session->control, 451, "No memory");
        return ESP_ERR_NO_MEM;
    }

    size_t total_received = 0;
    while (true) {
        int got = tls_socket_read(data, buffer, buffer_len);
        if (got == 0) {
            break;
        }
        if (got < 0) {
            err = ESP_FAIL;
            break;
        }
        total_received += got;
        if (archive_upload) {
            err = stream_upload_write(&upload, buffer, got);
            if (err != ESP_OK) {
                break;
            }
        }
    }

    free(buffer);
    tls_socket_close(data);
    free(data);

    if (archive_upload && err == ESP_OK) {
        err = stream_upload_finish(&upload);
    } else if (archive_upload) {
        stream_upload_abort(&upload);
    }
    if (archive_upload) {
        status_led_set(ESP_VP_STATUS_READY);
        if (err == ESP_OK) {
            status_led_pulse(ESP_VP_STATUS_CLIENT_ACTIVE, 1200);
        } else {
            status_led_pulse(ESP_VP_STATUS_ERROR, 1800);
        }
    }

    ESP_LOGI(TAG, "received file complete: %s bytes=%u err=%s archive=%d",
        filename,
        (unsigned)total_received,
        esp_err_to_name(err),
        archive_upload);
    ftp_send(&session->control, err == ESP_OK ? 226 : 451, err == ESP_OK ? "Transfer complete" : "Transfer failed");
    return err;
}

static void ftp_client_task(void *arg)
{
    ftp_session_t *session = calloc(1, sizeof(*session));
    int control_fd = (int)(intptr_t)arg;
    if (session == NULL) {
        close(control_fd);
        vTaskDelete(NULL);
        return;
    }
    session->passive_listener = -1;
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    if (getpeername(control_fd, (struct sockaddr *)&peer, &peer_len) == 0) {
        strlcpy(session->source_ip, inet_ntoa(peer.sin_addr), sizeof(session->source_ip));
    }
    if (tls_socket_init(&session->control, control_fd, true) != ESP_OK) {
        close(control_fd);
        free(session);
        vTaskDelete(NULL);
        return;
    }
    status_led_pulse(ESP_VP_STATUS_CLIENT_ACTIVE, 900);

    bool authed = false;
    char line[256];
    ftp_send(&session->control, 220, "Bambuddy ESP virtual printer ready");

    while (true) {
        int len = tls_socket_read(&session->control, line, sizeof(line) - 1);
        if (len <= 0) {
            break;
        }
        line[len] = '\0';
        char *cr = strpbrk(line, "\r\n");
        if (cr) {
            *cr = '\0';
        }
        char *argp = strchr(line, ' ');
        if (argp) {
            *argp++ = '\0';
        } else {
            argp = "";
        }
        ESP_LOGI(TAG, "FTP <- %s%s%s", line, argp[0] ? " " : "", strcasecmp(line, "PASS") == 0 ? "********" : argp);

        if (strcasecmp(line, "USER") == 0) {
            ftp_send(&session->control, strcasecmp(argp, "bblp") == 0 ? 331 : 530, "Password required");
        } else if (strcasecmp(line, "PASS") == 0) {
            authed = strcmp(argp, esp_vp_access_code()) == 0;
            ftp_send(&session->control, authed ? 230 : 530, authed ? "Login successful" : "Login incorrect");
            if (authed) {
                ESP_LOGI(TAG, "FTP login from %s", session->source_ip);
                status_led_pulse(ESP_VP_STATUS_CLIENT_ACTIVE, 1200);
            } else {
                ESP_LOGW(TAG, "FTP failed login from %s", session->source_ip);
                status_led_pulse(ESP_VP_STATUS_ERROR, 1200);
            }
        } else if (!authed) {
            ftp_send(&session->control, 530, "Not logged in");
        } else if (strcasecmp(line, "SYST") == 0) {
            ftp_send(&session->control, 215, "UNIX Type: L8");
        } else if (strcasecmp(line, "TYPE") == 0 || strcasecmp(line, "PBSZ") == 0 || strcasecmp(line, "PROT") == 0) {
            ftp_send(&session->control, 200, "OK");
        } else if (strcasecmp(line, "PWD") == 0) {
            ftp_send(&session->control, 257, "\"/\" is current directory");
        } else if (strcasecmp(line, "CWD") == 0 || strcasecmp(line, "OPTS") == 0) {
            ftp_send(&session->control, 250, "OK");
        } else if (strcasecmp(line, "PASV") == 0) {
            handle_pasv(session);
        } else if (strcasecmp(line, "EPSV") == 0) {
            handle_epsv(session);
        } else if (strcasecmp(line, "STOR") == 0) {
            const char *slash = strrchr(argp, '/');
            const char *filename = slash ? slash + 1 : argp;
            relay_stor_data(session, filename && filename[0] ? filename : "upload.3mf");
        } else if (strcasecmp(line, "SIZE") == 0) {
            ftp_send(&session->control, 550, "File not found");
        } else if (strcasecmp(line, "QUIT") == 0) {
            ftp_send(&session->control, 221, "Goodbye");
            break;
        } else {
            ftp_send(&session->control, 502, "Command not implemented");
        }
    }

    close_passive(session);
    tls_socket_close(&session->control);
    free(session);
    vTaskDelete(NULL);
}

static void ftps_task(void *arg)
{
    int listener = make_listener(ESP_VP_FTPS_PORT, 2);
    if (listener < 0) {
        ESP_LOGW(TAG, "port %d unavailable: errno=%d", ESP_VP_FTPS_PORT, errno);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "listening TCP/%d", ESP_VP_FTPS_PORT);
    while (true) {
        struct sockaddr_in peer = {0};
        socklen_t peer_len = sizeof(peer);
        int client = accept(listener, (struct sockaddr *)&peer, &peer_len);
        if (client >= 0) {
            ESP_LOGI(TAG, "accepted TCP/%d from %s", ESP_VP_FTPS_PORT, inet_ntoa(peer.sin_addr));
            xTaskCreate(ftp_client_task, "ftp_client", 8192, (void *)(intptr_t)client, 5, NULL);
        }
    }
}

void ftps_server_start(void)
{
    xTaskCreate(ftps_task, "ftps", 8192, NULL, 5, NULL);
}
