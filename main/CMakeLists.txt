idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES relay wifi_mgr http_api button esp_app_format
)
