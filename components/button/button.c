#include "button.h"

#include "esp_log.h"

static const char *TAG = "button";

esp_err_t button_init(void)
{
    // Заглушка скелета: антидребезг и обработка нажатий — в следующем тикете.
    ESP_LOGD(TAG, "button_init: stub");
    return ESP_OK;
}
