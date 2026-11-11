#include "relay.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

// ADR-0005: активный низкий уровень (ON = 0 В, OFF = 3.3 В).
#define RELAY_ACTIVE_LEVEL 0
#define RELAY_INACTIVE_LEVEL 1

static const char *TAG = "relay";

static SemaphoreHandle_t s_mutex;
static relay_state_t s_state = RELAY_STATE_OFF;

static int relay_gpio_level(relay_state_t state) {
  return state == RELAY_STATE_ON ? RELAY_ACTIVE_LEVEL : RELAY_INACTIVE_LEVEL;
}

static esp_err_t lock_state(void) {
  if (s_mutex == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  return ESP_OK;
}

static void unlock_state(void) {
  if (s_mutex != NULL) {
    xSemaphoreGive(s_mutex);
  }
}

esp_err_t relay_init(void) {
  if (s_mutex != NULL) {
    return ESP_OK;
  }

  s_mutex = xSemaphoreCreateMutex();
  if (s_mutex == NULL) {
    return ESP_ERR_NO_MEM;
  }

  gpio_config_t config = {
      .pin_bit_mask = 1ULL << CONFIG_RELAY_GPIO,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };

  // Сначала фиксируем безопасный уровень OFF в регистре выходов,
  // затем включаем выход, чтобы не было импульса при старте (ADR-0008).
  gpio_set_level(CONFIG_RELAY_GPIO, RELAY_INACTIVE_LEVEL);
  esp_err_t err = gpio_config(&config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
    return err;
  }

  err = gpio_set_level(CONFIG_RELAY_GPIO, RELAY_INACTIVE_LEVEL);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set safe state: %s", esp_err_to_name(err));
    return err;
  }

  if (lock_state() != ESP_OK) {
    return ESP_ERR_INVALID_STATE;
  }
  s_state = RELAY_STATE_OFF;
  unlock_state();

  ESP_LOGI(TAG, "Relay initialized on GPIO %d, state OFF", CONFIG_RELAY_GPIO);
  return ESP_OK;
}

relay_state_t relay_get_state(void) {
  if (lock_state() != ESP_OK) {
    return RELAY_STATE_OFF;
  }

  relay_state_t state = s_state;
  unlock_state();
  return state;
}

esp_err_t relay_set_state(relay_state_t state) {
  if (state != RELAY_STATE_OFF && state != RELAY_STATE_ON) {
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t err = lock_state();
  if (err != ESP_OK) {
    return err;
  }

  // Чтение — изменение — запись GPIO под мьютексом (ADR-0007).
  err = gpio_set_level(CONFIG_RELAY_GPIO, relay_gpio_level(state));
  if (err == ESP_OK) {
    s_state = state;
  }
  unlock_state();

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set relay state: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "Relay %s", state == RELAY_STATE_ON ? "ON" : "OFF");
  }
  return err;
}

esp_err_t relay_toggle(void) {
  esp_err_t err = lock_state();
  if (err != ESP_OK) {
    return err;
  }

  relay_state_t next =
      s_state == RELAY_STATE_ON ? RELAY_STATE_OFF : RELAY_STATE_ON;
  // Чтение — изменение — запись GPIO под мьютексом (ADR-0007).
  err = gpio_set_level(CONFIG_RELAY_GPIO, relay_gpio_level(next));
  if (err == ESP_OK) {
    s_state = next;
  }
  unlock_state();

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to toggle relay: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "Relay %s", next == RELAY_STATE_ON ? "ON" : "OFF");
  }
  return err;
}
