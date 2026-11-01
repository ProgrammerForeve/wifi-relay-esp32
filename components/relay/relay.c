#include "relay.h"

#include "esp_log.h"

static const char *TAG = "relay";

esp_err_t relay_init(void)
{
    // Заглушка скелета: реальная настройка GPIO и мьютекса — в следующем тикете.
    ESP_LOGD(TAG, "relay_init: stub");
    return ESP_OK;
}

relay_state_t relay_get_state(void)
{
    return RELAY_STATE_OFF;
}

esp_err_t relay_set_state(relay_state_t state)
{
    (void)state;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t relay_toggle(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
