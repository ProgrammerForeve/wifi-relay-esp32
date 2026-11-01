#include "esp_app_desc.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "button.h"
#include "http_api.h"
#include "relay.h"
#include "wifi_mgr.h"

static const char *TAG = "main";

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();

    ESP_LOGI(TAG, "ESP32 Wi-Fi Relay v%s starting", app->version);
    ESP_LOGI(TAG, "target=%s relay_gpio=%d button_gpio=%d led_gpio=%d",
             CONFIG_IDF_TARGET, CONFIG_RELAY_GPIO, CONFIG_BUTTON_GPIO,
             CONFIG_LED_GPIO);

    // Порядок важен: сначала безопасное состояние реле (ADR-0008),
    // затем остальные подсистемы.
    ESP_ERROR_CHECK(relay_init());
    ESP_ERROR_CHECK(button_init());
    ESP_ERROR_CHECK(wifi_mgr_init());
    ESP_ERROR_CHECK(http_api_start());

    ESP_LOGI(TAG, "initialization complete");
}
