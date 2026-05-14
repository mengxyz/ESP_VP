#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

typedef struct {
    int fd;
    bool tls;
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cert;
    mbedtls_pk_context key;
} tls_socket_t;

esp_err_t tls_socket_init(tls_socket_t *sock, int fd, bool enable_tls);
int tls_socket_read(tls_socket_t *sock, void *buf, size_t len);
int tls_socket_write(tls_socket_t *sock, const void *buf, size_t len);
void tls_socket_close(tls_socket_t *sock);
