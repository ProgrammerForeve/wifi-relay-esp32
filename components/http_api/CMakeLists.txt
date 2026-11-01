idf_component_register(
    SRCS "http_api.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server relay wifi_mgr
)
