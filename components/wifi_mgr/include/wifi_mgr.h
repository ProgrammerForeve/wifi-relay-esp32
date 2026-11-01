#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Текущий режим Wi-Fi (ADR-0004).
 */
typedef enum {
    WIFI_MGR_MODE_OFF = 0,
    WIFI_MGR_MODE_STA,
    WIFI_MGR_MODE_AP,
} wifi_mgr_mode_t;

/**
 * @brief Снимок диагностических данных для GET /api/status.
 *
 * Не содержит токен и Wi-Fi-пароль (ADR-0002).
 */
typedef struct {
    wifi_mgr_mode_t mode;
    char ip[16];
    int8_t rssi;
} wifi_mgr_info_t;

/**
 * @brief Прочитать креденшелы из NVS и запустить STA с fallback в AP.
 */
esp_err_t wifi_mgr_init(void);

/**
 * @brief Текущий режим Wi-Fi.
 */
wifi_mgr_mode_t wifi_mgr_get_mode(void);

/**
 * @brief Заполнить снимок диагностики; null-указатель — ошибка.
 */
esp_err_t wifi_mgr_get_info(wifi_mgr_info_t *info);

/**
 * @brief Записать SSID/пароль в NVS и перейти в STA (ADR-0006).
 */
esp_err_t wifi_mgr_provision(const char *ssid, const char *password);

/**
 * @brief Стереть Wi-Fi-креденшелы из NVS и перейти в AP.
 */
esp_err_t wifi_mgr_reset_credentials(void);

#ifdef __cplusplus
}
#endif
