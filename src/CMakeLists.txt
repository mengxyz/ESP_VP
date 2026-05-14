idf_component_register(
    SRCS
        "main.c"
        "wifi.c"
        "ssdp.c"
        "bind_server.c"
        "mqtt_server.c"
        "stream_upload.c"
        "tls_socket.c"
        "ftps_server.c"
    INCLUDE_DIRS "../include"
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        mbedtls
)
