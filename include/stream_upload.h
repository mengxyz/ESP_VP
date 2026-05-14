#pragma once

#include <stddef.h>

#include "esp_err.h"

typedef struct {
    int sock;
    char filename[128];
    char source_ip[48];
    char response_buf[192];
    size_t bytes;
} stream_upload_t;

esp_err_t stream_upload_begin(stream_upload_t *upload, const char *filename, const char *source_ip);
esp_err_t stream_upload_write(stream_upload_t *upload, const unsigned char *data, size_t len);
esp_err_t stream_upload_finish(stream_upload_t *upload);
void stream_upload_abort(stream_upload_t *upload);
