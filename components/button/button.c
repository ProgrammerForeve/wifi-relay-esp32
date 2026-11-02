#include "button.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "relay.h"
#include "sdkconfig.h"
#include "wifi_mgr.h"

#define BUTTON_POLL_INTERVAL_MS 10
#define BUTTON_DEBOUNCE_US (50LL * 1000)
#define BUTTON_SHORT_PRESS_MAX_US (1LL * 1000 * 1000)
#define BUTTON_LONG_PRESS_US (5LL * 1000 * 1000)
#define BUTTON_TASK_STACK 4096
#define BUTTON_TASK_PRIO 5

static const char *TAG = "button";

static bool s_started;

button_debounce_t button_debounce_step(button_debounce_t state,
                                       bool raw_pressed, int64_t now_us,
                                       int64_t debounce_us) {
  if (raw_pressed != state.raw_pressed) {
    state.raw_pressed = raw_pressed;
    state.raw_changed_us = now_us;
  }

  if (state.raw_pressed != state.stable_pressed &&
      (now_us - state.raw_changed_us) >= debounce_us) {
    state.stable_pressed = state.raw_pressed;
  }

  return state;
}

static void button_task(void *arg) {
  (void)arg;

  button_debounce_t debounce = {0};
  int64_t press_start_us = 0;
  bool long_press_handled = false;

  while (true) {
    bool raw_pressed = gpio_get_level(CONFIG_BUTTON_GPIO) == 0;
    int64_t now_us = esp_timer_get_time();
    bool was_pressed = debounce.stable_pressed;

    debounce =
        button_debounce_step(debounce, raw_pressed, now_us, BUTTON_DEBOUNCE_US);

    if (!was_pressed && debounce.stable_pressed) {
      press_start_us = now_us;
      long_press_handled = false;
    } else if (was_pressed && !debounce.stable_pressed) {
      int64_t duration_us = now_us - press_start_us;
      if (!long_press_handled && duration_us < BUTTON_SHORT_PRESS_MAX_US) {
        ESP_LOGI(TAG, "Short press, toggling relay");
        esp_err_t err = relay_toggle();
        if (err != ESP_OK) {
          ESP_LOGE(TAG, "relay_toggle failed: %s", esp_err_to_name(err));
        }
      }
      long_press_handled = false;
    } else if (debounce.stable_pressed && !long_press_handled &&
               (now_us - press_start_us) >= BUTTON_LONG_PRESS_US) {
      long_press_handled = true;
      ESP_LOGW(TAG, "Long press, resetting Wi-Fi credentials");
      esp_err_t err = wifi_mgr_reset_credentials();
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_mgr_reset_credentials failed: %s",
                 esp_err_to_name(err));
      }
    }

    vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_INTERVAL_MS));
  }
}

esp_err_t button_init(void) {
  if (s_started) {
    return ESP_OK;
  }

  gpio_config_t config = {
      .pin_bit_mask = 1ULL << CONFIG_BUTTON_GPIO,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };

  esp_err_t err = gpio_config(&config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
    return err;
  }

  if (xTaskCreate(button_task, "button", BUTTON_TASK_STACK, NULL,
                  BUTTON_TASK_PRIO, NULL) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create button task");
    return ESP_ERR_NO_MEM;
  }

  s_started = true;
  ESP_LOGI(TAG, "Button initialized on GPIO %d", CONFIG_BUTTON_GPIO);
  return ESP_OK;
}
