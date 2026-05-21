idf_component_register(
    SRCS
        "main.c"
        "wifi.c"
        "config_store.c"
        "pump_client.c"
        "state.c"
        "ui.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_client
        json
)
