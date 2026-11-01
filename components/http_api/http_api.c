#include "http_api.h"

#include "esp_log.h"

static const char *TAG = "http_api";

esp_err_t http_api_start(void)
{
    // Заглушка скелета: обработчики и esp_http_server — в следующих тикетах.
    ESP_LOGD(TAG, "http_api_start: stub");
    return ESP_OK;
}

esp_err_t http_api_stop(void)
{
    return ESP_OK;
}
