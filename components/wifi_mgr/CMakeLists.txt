idf_component_register(
    SRCS "wifi_mgr.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi nvs_flash esp_event esp_netif esp_timer esp_hw_support
)
