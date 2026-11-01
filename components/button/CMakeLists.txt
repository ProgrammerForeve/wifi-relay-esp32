idf_component_register(
    SRCS "button.c"
    INCLUDE_DIRS "include"
    REQUIRES relay wifi_mgr esp_driver_gpio
)
