#include "wifi_mgr.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "wifi_mgr";

esp_err_t wifi_mgr_init(void)
{
    // Заглушка скелета: STA/AP/backoff/NVS — в следующем тикете.
    ESP_LOGD(TAG, "wifi_mgr_init: stub");
    return ESP_OK;
}

wifi_mgr_mode_t wifi_mgr_get_mode(void)
{
    return WIFI_MGR_MODE_OFF;
}

esp_err_t wifi_mgr_get_info(wifi_mgr_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));
    info->mode = WIFI_MGR_MODE_OFF;
    info->rssi = 0;
    return ESP_OK;
}

esp_err_t wifi_mgr_provision(const char *ssid, const char *password)
{
    (void)ssid;
    (void)password;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wifi_mgr_reset_credentials(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
