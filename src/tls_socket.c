#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "mbedtls/error.h"

#include "app_config.h"
#include "tls_socket.h"

static const char *TAG = "tls_socket";

static bool tls_configured(void)
{
    return strlen(APP_TLS_CERT_PEM) > 0 && strlen(APP_TLS_KEY_PEM) > 0;
}

static void log_mbedtls_error(const char *what, int ret)
{
    char err[128];
    mbedtls_strerror(ret, err, sizeof(err));
    ESP_LOGE(TAG, "%s failed: -0x%04x %s", what, (unsigned)-ret, err);
}

static void tls_socket_free_tls(tls_socket_t *sock, bool notify)
{
    if (notify) {
        mbedtls_ssl_close_notify(&sock->ssl);
    }
    mbedtls_ssl_free(&sock->ssl);
    mbedtls_ssl_config_free(&sock->conf);
    mbedtls_x509_crt_free(&sock->cert);
    mbedtls_pk_free(&sock->key);
    mbedtls_net_free(&sock->net);
    sock->fd = -1;
    sock->tls = false;
}

esp_err_t tls_socket_init(tls_socket_t *sock, int fd, bool enable_tls)
{
    memset(sock, 0, sizeof(*sock));
    sock->fd = fd;
    sock->tls = enable_tls && tls_configured();
    if (!sock->tls) {
        return ESP_OK;
    }

    mbedtls_net_init(&sock->net);
    mbedtls_ssl_init(&sock->ssl);
    mbedtls_ssl_config_init(&sock->conf);
    mbedtls_x509_crt_init(&sock->cert);
    mbedtls_pk_init(&sock->key);
    sock->net.fd = fd;

    int ret = mbedtls_x509_crt_parse(&sock->cert, (const unsigned char *)APP_TLS_CERT_PEM, strlen(APP_TLS_CERT_PEM) + 1);
    if (ret != 0) {
        log_mbedtls_error("x509_crt_parse", ret);
        tls_socket_free_tls(sock, false);
        return ESP_FAIL;
    }
    ret = mbedtls_pk_parse_key(
        &sock->key,
        (const unsigned char *)APP_TLS_KEY_PEM,
        strlen(APP_TLS_KEY_PEM) + 1,
        NULL,
        0);
    if (ret != 0) {
        log_mbedtls_error("pk_parse_key", ret);
        tls_socket_free_tls(sock, false);
        return ESP_FAIL;
    }
    ret = mbedtls_ssl_config_defaults(&sock->conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        log_mbedtls_error("ssl_config_defaults", ret);
        tls_socket_free_tls(sock, false);
        return ESP_FAIL;
    }
    mbedtls_ssl_conf_min_tls_version(&sock->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&sock->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    ret = mbedtls_ssl_conf_own_cert(&sock->conf, &sock->cert, &sock->key);
    if (ret != 0) {
        log_mbedtls_error("ssl_conf_own_cert", ret);
        tls_socket_free_tls(sock, false);
        return ESP_FAIL;
    }
    ret = mbedtls_ssl_setup(&sock->ssl, &sock->conf);
    if (ret != 0) {
        log_mbedtls_error("ssl_setup", ret);
        tls_socket_free_tls(sock, false);
        return ESP_FAIL;
    }
    mbedtls_ssl_set_bio(&sock->ssl, &sock->net, mbedtls_net_send, mbedtls_net_recv, NULL);

    do {
        ret = mbedtls_ssl_handshake(&sock->ssl);
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (ret != 0) {
        log_mbedtls_error("ssl_handshake", ret);
        tls_socket_free_tls(sock, false);
        return ESP_FAIL;
    }
    return ESP_OK;
}

int tls_socket_read(tls_socket_t *sock, void *buf, size_t len)
{
    if (!sock->tls) {
        return recv(sock->fd, buf, len, 0);
    }
    int ret;
    do {
        ret = mbedtls_ssl_read(&sock->ssl, buf, len);
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        return 0;
    }
    return ret;
}

int tls_socket_write(tls_socket_t *sock, const void *buf, size_t len)
{
    if (!sock->tls) {
        return send(sock->fd, buf, len, 0);
    }
    int ret;
    size_t sent = 0;
    while (sent < len) {
        do {
            ret = mbedtls_ssl_write(&sock->ssl, (const unsigned char *)buf + sent, len - sent);
        } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
        if (ret <= 0) {
            return ret;
        }
        sent += ret;
    }
    return sent;
}

void tls_socket_close(tls_socket_t *sock)
{
    if (sock->tls) {
        tls_socket_free_tls(sock, true);
    } else if (sock->fd >= 0) {
        close(sock->fd);
        sock->fd = -1;
    }
}
