idf_component_register(
    SRCS
        "main.c"
        "expansion.c"
        "tcp_server.c"
        "wifi_manager.c"
        "config.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        mdns
        lwip
        driver
)
