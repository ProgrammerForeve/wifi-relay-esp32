#include "wifi_mgr.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_NVS_KEY_SSID "ssid"
#define WIFI_NVS_KEY_PASSWORD "password"

#define WIFI_SSID_SIZE 32
#define WIFI_PASSWORD_SIZE 64
#define WIFI_SSID_BUFFER_SIZE (WIFI_SSID_SIZE + 1)
#define WIFI_PASSWORD_BUFFER_SIZE (WIFI_PASSWORD_SIZE + 1)

#define STA_CONNECT_TIMEOUT_US (30LL * 1000 * 1000)
#define STA_FAIL_AP_TIMEOUT_US (5LL * 60 * 1000 * 1000)
#define BACKOFF_INIT_MS 1000U
#define BACKOFF_MAX_MS 60000U

#define WIFI_CMD_QUEUE_LEN 8
#define WIFI_CMD_TASK_STACK 4096
#define WIFI_CMD_TASK_PRIO 5

static const char *TAG = "wifi_mgr";

typedef enum {
  WIFI_CMD_RECONNECT = 0,
  WIFI_CMD_START_AP,
} wifi_cmd_t;

static SemaphoreHandle_t s_state_mutex;
static SemaphoreHandle_t s_transition_mutex;
static QueueHandle_t s_cmd_queue;
static esp_timer_handle_t s_connect_timeout_timer;
static esp_timer_handle_t s_reconnect_timer;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;

static wifi_mgr_mode_t s_mode = WIFI_MGR_MODE_OFF;
static bool s_connected;
static bool s_ever_connected;
static uint32_t s_backoff_ms = BACKOFF_INIT_MS;
static int64_t s_first_fail_us;
static char s_ip[16];
static int8_t s_rssi;
static char s_ap_ssid[33];

static void state_lock(void) {
  if (s_state_mutex != NULL) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
  }
}

static void state_unlock(void) {
  if (s_state_mutex != NULL) {
    xSemaphoreGive(s_state_mutex);
  }
}

static void transition_lock(void) {
  if (s_transition_mutex != NULL) {
    xSemaphoreTake(s_transition_mutex, portMAX_DELAY);
  }
}

static void transition_unlock(void) {
  if (s_transition_mutex != NULL) {
    xSemaphoreGive(s_transition_mutex);
  }
}

static esp_err_t load_credentials(char *ssid, size_t ssid_size, char *password,
                                  size_t password_size) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK) {
    return err;
  }

  size_t len = ssid_size;
  err = nvs_get_str(nvs, WIFI_NVS_KEY_SSID, ssid, &len);
  if (err == ESP_OK) {
    len = password_size;
    err = nvs_get_str(nvs, WIFI_NVS_KEY_PASSWORD, password, &len);
  }
  nvs_close(nvs);

  if (err == ESP_OK && ssid[0] == '\0') {
    return ESP_ERR_NOT_FOUND;
  }
  return err;
}

static esp_err_t save_credentials(const char *ssid, const char *password) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    return err;
  }

  err = nvs_set_str(nvs, WIFI_NVS_KEY_SSID, ssid);
  if (err == ESP_OK) {
    err = nvs_set_str(nvs, WIFI_NVS_KEY_PASSWORD, password);
  }
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);
  return err;
}

static void post_command(wifi_cmd_t cmd) {
  if (s_cmd_queue == NULL) {
    return;
  }
  if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
    ESP_LOGW(TAG, "command queue is full");
  }
}

static uint32_t take_backoff_ms(void) {
  uint32_t current = s_backoff_ms;
  s_backoff_ms = (current >= BACKOFF_MAX_MS) ? BACKOFF_MAX_MS : current * 2;
  return current;
}

static void connect_timeout_cb(void *arg) {
  (void)arg;

  bool pending;
  state_lock();
  pending = (s_mode == WIFI_MGR_MODE_STA && !s_connected);
  state_unlock();

  if (pending) {
    ESP_LOGW(TAG, "STA connection timeout (30 s), switching to AP");
    post_command(WIFI_CMD_START_AP);
  }
}

static void reconnect_cb(void *arg) {
  (void)arg;

  bool pending;
  state_lock();
  pending = (s_mode == WIFI_MGR_MODE_STA && !s_connected);
  state_unlock();

  if (pending) {
    post_command(WIFI_CMD_RECONNECT);
  }
}

static void handle_got_ip(ip_event_got_ip_t *event) {
  wifi_ap_record_t ap_info = {0};
  int8_t rssi = 0;
  if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
    rssi = ap_info.rssi;
  }

  state_lock();
  if (s_mode != WIFI_MGR_MODE_STA) {
    state_unlock();
    return;
  }
  s_connected = true;
  s_ever_connected = true;
  s_backoff_ms = BACKOFF_INIT_MS;
  s_first_fail_us = 0;
  s_rssi = rssi;
  snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
  state_unlock();

  esp_timer_stop(s_connect_timeout_timer);
  esp_timer_stop(s_reconnect_timer);

  ESP_LOGI(TAG, "STA connected, IP: " IPSTR ", RSSI: %d dBm",
           IP2STR(&event->ip_info.ip), rssi);
}

static void
handle_sta_disconnected(const wifi_event_sta_disconnected_t *event) {
  ESP_LOGW(TAG, "STA disconnected, reason=%d", event->reason);

  bool switch_to_ap = false;
  uint32_t delay_ms = 0;
  int64_t now = esp_timer_get_time();

  state_lock();
  if (s_mode != WIFI_MGR_MODE_STA) {
    state_unlock();
    return;
  }
  s_connected = false;
  s_ip[0] = '\0';
  s_rssi = 0;

  if (s_first_fail_us == 0) {
    s_first_fail_us = now;
  }

  // После успешного подключения ждём ~5 минут; до первого успеха
  // переход в AP выполняет таймаут 30 секунд.
  if (s_ever_connected && (now - s_first_fail_us) >= STA_FAIL_AP_TIMEOUT_US) {
    switch_to_ap = true;
  } else {
    delay_ms = take_backoff_ms();
  }
  state_unlock();

  if (switch_to_ap) {
    ESP_LOGW(TAG, "No STA connection for ~5 min, switching to AP");
    post_command(WIFI_CMD_START_AP);
    return;
  }

  ESP_LOGW(TAG, "Reconnect in %u ms", (unsigned)delay_ms);
  esp_timer_stop(s_reconnect_timer);
  esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000);
}

static void handle_ap_start(void) {
  esp_netif_ip_info_t ip_info;
  memset(&ip_info, 0, sizeof(ip_info));

  if (s_ap_netif != NULL &&
      esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
    state_lock();
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip_info.ip));
    state_unlock();
    ESP_LOGI(TAG, "AP ready, IP: " IPSTR, IP2STR(&ip_info.ip));
  } else {
    ESP_LOGI(TAG, "AP ready");
  }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  (void)arg;

  if (event_base == WIFI_EVENT) {
    switch (event_id) {
    case WIFI_EVENT_STA_START:
      post_command(WIFI_CMD_RECONNECT);
      break;
    case WIFI_EVENT_STA_DISCONNECTED:
      handle_sta_disconnected(
          (const wifi_event_sta_disconnected_t *)event_data);
      break;
    case WIFI_EVENT_AP_START:
      handle_ap_start();
      break;
    default:
      break;
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    handle_got_ip((ip_event_got_ip_t *)event_data);
  }
}

static esp_err_t start_sta(void) {
  char ssid[WIFI_SSID_BUFFER_SIZE] = {0};
  char password[WIFI_PASSWORD_BUFFER_SIZE] = {0};

  esp_err_t err =
      load_credentials(ssid, sizeof(ssid), password, sizeof(password));
  if (err != ESP_OK) {
    return err;
  }

  size_t ssid_len = strlen(ssid);
  size_t password_len = strlen(password);
  if (ssid_len > WIFI_SSID_SIZE || password_len > WIFI_PASSWORD_SIZE) {
    return ESP_ERR_INVALID_SIZE;
  }

  transition_lock();

  state_lock();
  s_mode = WIFI_MGR_MODE_OFF;
  s_connected = false;
  s_ever_connected = false;
  s_backoff_ms = BACKOFF_INIT_MS;
  s_first_fail_us = 0;
  s_ip[0] = '\0';
  s_rssi = 0;
  s_ap_ssid[0] = '\0';
  state_unlock();

  esp_timer_stop(s_reconnect_timer);
  esp_timer_stop(s_connect_timeout_timer);

  // Останавливаем Wi-Fi до смены режима, чтобы события старого
  // подключения не запустили backoff.
  (void)esp_wifi_stop();

  wifi_config_t config = {0};
  memcpy(config.sta.ssid, ssid, ssid_len);
  memcpy(config.sta.password, password, password_len);

  state_lock();
  s_mode = WIFI_MGR_MODE_STA;
  state_unlock();

  err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err == ESP_OK) {
    err = esp_wifi_set_config(WIFI_IF_STA, &config);
  }
  if (err == ESP_OK) {
    esp_timer_start_once(s_connect_timeout_timer, STA_CONNECT_TIMEOUT_US);
    err = esp_wifi_start();
    if (err != ESP_OK) {
      esp_timer_stop(s_connect_timeout_timer);
    }
  }

  if (err != ESP_OK) {
    state_lock();
    s_mode = WIFI_MGR_MODE_OFF;
    state_unlock();
    transition_unlock();
    ESP_LOGE(TAG, "Failed to start STA: %s", esp_err_to_name(err));
    return err;
  }

  transition_unlock();

  ESP_LOGI(TAG, "STA started, connecting (timeout 30 s)");
  return ESP_OK;
}

static esp_err_t start_ap(void) {
  transition_lock();

  state_lock();
  if (s_mode == WIFI_MGR_MODE_AP) {
    state_unlock();
    transition_unlock();
    return ESP_OK;
  }
  s_mode = WIFI_MGR_MODE_OFF;
  s_connected = false;
  s_ever_connected = false;
  s_ip[0] = '\0';
  s_rssi = 0;
  state_unlock();

  esp_timer_stop(s_connect_timeout_timer);
  esp_timer_stop(s_reconnect_timer);

  uint8_t mac[6] = {0};
  esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  if (err != ESP_OK) {
    transition_unlock();
    ESP_LOGE(TAG, "Failed to read MAC: %s", esp_err_to_name(err));
    return err;
  }

  char ssid[WIFI_SSID_BUFFER_SIZE] = {0};
  snprintf(ssid, sizeof(ssid), "ESP32-Relay-%02X%02X", mac[4], mac[5]);

  const char *password = CONFIG_RELAY_AP_PASSWORD;
  size_t ssid_len = strlen(ssid);
  size_t password_len = strlen(password);
  if (ssid_len > WIFI_SSID_SIZE || password_len > WIFI_PASSWORD_SIZE ||
      password_len < 8) {
    transition_unlock();
    ESP_LOGE(TAG, "Invalid AP configuration");
    return ESP_ERR_INVALID_ARG;
  }

  wifi_config_t config = {0};
  memcpy(config.ap.ssid, ssid, ssid_len);
  config.ap.ssid_len = (uint8_t)ssid_len;
  memcpy(config.ap.password, password, password_len);
  config.ap.channel = 1;
  config.ap.max_connection = 4;
  config.ap.authmode = WIFI_AUTH_WPA2_PSK;

  // Останавливаем Wi-Fi до смены режима, чтобы события старого
  // подключения не запустили backoff.
  (void)esp_wifi_stop();

  state_lock();
  s_mode = WIFI_MGR_MODE_AP;
  snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", ssid);
  state_unlock();

  err = esp_wifi_set_mode(WIFI_MODE_AP);
  if (err == ESP_OK) {
    err = esp_wifi_set_config(WIFI_IF_AP, &config);
  }
  if (err == ESP_OK) {
    err = esp_wifi_start();
  }

  if (err != ESP_OK) {
    state_lock();
    s_mode = WIFI_MGR_MODE_OFF;
    state_unlock();
    transition_unlock();
    ESP_LOGE(TAG, "Failed to start AP: %s", esp_err_to_name(err));
    return err;
  }

  transition_unlock();

  ESP_LOGW(TAG, "AP started: SSID=%s", ssid);
  return ESP_OK;
}

static void wifi_cmd_task(void *arg) {
  (void)arg;

  wifi_cmd_t cmd;
  while (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
    switch (cmd) {
    case WIFI_CMD_RECONNECT: {
      bool should_connect;
      state_lock();
      should_connect = (s_mode == WIFI_MGR_MODE_STA && !s_connected);
      state_unlock();

      if (should_connect) {
        ESP_LOGW(TAG, "STA reconnect attempt");
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
          ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        }
      }
      break;
    }
    case WIFI_CMD_START_AP: {
      bool connected;
      state_lock();
      connected = s_connected;
      state_unlock();

      if (!connected) {
        start_ap();
      }
      break;
    }
    default:
      break;
    }
  }

  vTaskDelete(NULL);
}

esp_err_t wifi_mgr_init(void) {
  if (s_state_mutex != NULL) {
    return ESP_OK;
  }

  s_state_mutex = xSemaphoreCreateMutex();
  s_transition_mutex = xSemaphoreCreateMutex();
  s_cmd_queue = xQueueCreate(WIFI_CMD_QUEUE_LEN, sizeof(wifi_cmd_t));
  if (s_state_mutex == NULL || s_transition_mutex == NULL ||
      s_cmd_queue == NULL) {
    return ESP_ERR_NO_MEM;
  }

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS init failed (%s), erasing", esp_err_to_name(err));
    err = nvs_flash_erase();
    if (err == ESP_OK) {
      err = nvs_flash_init();
    }
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
    return err;
  }

  err = esp_netif_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
    return err;
  }

  err = esp_event_loop_create_default();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "event loop init failed: %s", esp_err_to_name(err));
    return err;
  }

  s_sta_netif = esp_netif_create_default_wifi_sta();
  s_ap_netif = esp_netif_create_default_wifi_ap();
  if (s_sta_netif == NULL || s_ap_netif == NULL) {
    ESP_LOGE(TAG, "Failed to create network interfaces");
    return ESP_ERR_NO_MEM;
  }

  wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&wifi_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
    return err;
  }

  err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_set_storage failed: %s", esp_err_to_name(err));
    return err;
  }

  err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            wifi_event_handler, NULL, NULL);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "WIFI_EVENT handler registration failed: %s",
             esp_err_to_name(err));
    return err;
  }

  err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            wifi_event_handler, NULL, NULL);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "IP_EVENT handler registration failed: %s",
             esp_err_to_name(err));
    return err;
  }

  const esp_timer_create_args_t timeout_args = {
      .callback = connect_timeout_cb,
      .arg = NULL,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "wifi_connect_timeout",
  };
  err = esp_timer_create(&timeout_args, &s_connect_timeout_timer);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "connect timeout timer creation failed: %s",
             esp_err_to_name(err));
    return err;
  }

  const esp_timer_create_args_t reconnect_args = {
      .callback = reconnect_cb,
      .arg = NULL,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "wifi_reconnect",
  };
  err = esp_timer_create(&reconnect_args, &s_reconnect_timer);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "reconnect timer creation failed: %s", esp_err_to_name(err));
    return err;
  }

  if (xTaskCreate(wifi_cmd_task, "wifi_mgr", WIFI_CMD_TASK_STACK, NULL,
                  WIFI_CMD_TASK_PRIO, NULL) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create Wi-Fi control task");
    return ESP_ERR_NO_MEM;
  }

  char ssid[WIFI_SSID_BUFFER_SIZE] = {0};
  char password[WIFI_PASSWORD_BUFFER_SIZE] = {0};
  err = load_credentials(ssid, sizeof(ssid), password, sizeof(password));
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "NVS credentials found, starting STA");
    err = start_sta();
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "STA start failed, falling back to AP");
      return start_ap();
    }
    return ESP_OK;
  }

  if (err != ESP_ERR_NVS_NOT_FOUND && err != ESP_ERR_NOT_FOUND) {
    ESP_LOGW(TAG, "Failed to load Wi-Fi credentials: %s", esp_err_to_name(err));
  }

  ESP_LOGW(TAG, "No Wi-Fi credentials in NVS, starting AP");
  return start_ap();
}

wifi_mgr_mode_t wifi_mgr_get_mode(void) {
  wifi_mgr_mode_t mode;

  state_lock();
  mode = s_mode;
  state_unlock();
  return mode;
}

esp_err_t wifi_mgr_get_info(wifi_mgr_info_t *info) {
  if (info == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  state_lock();
  info->mode = s_mode;
  memcpy(info->ip, s_ip, sizeof(info->ip));
  info->rssi = s_rssi;
  if (s_mode == WIFI_MGR_MODE_AP) {
    memcpy(info->ssid, s_ap_ssid, sizeof(info->ssid));
  } else {
    info->ssid[0] = '\0';
  }
  state_unlock();
  return ESP_OK;
}

uint32_t wifi_mgr_get_uptime_s(void) {
  return (uint32_t)(esp_timer_get_time() / 1000000);
}

esp_err_t wifi_mgr_provision(const char *ssid, const char *password) {
  if (ssid == NULL || password == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  size_t ssid_len = strlen(ssid);
  size_t password_len = strlen(password);
  if (ssid_len == 0 || ssid_len > WIFI_SSID_SIZE ||
      password_len > WIFI_PASSWORD_SIZE) {
    return ESP_ERR_INVALID_ARG;
  }

  if (s_state_mutex == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  esp_err_t err = save_credentials(ssid, password);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to save credentials: %s", esp_err_to_name(err));
    return err;
  }

  ESP_LOGI(TAG, "Wi-Fi credentials saved");
  err = start_sta();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "STA start after provisioning failed, starting AP");
    return start_ap();
  }
  return ESP_OK;
}

esp_err_t wifi_mgr_reset_credentials(void) {
  // Стирание креденшелов и возврат в AP — в тикете про provisioning.
  return ESP_ERR_NOT_SUPPORTED;
}
