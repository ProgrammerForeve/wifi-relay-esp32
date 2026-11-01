#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Запустить HTTP-сервер с REST API и captive-страницей.
 */
esp_err_t http_api_start(void);

/**
 * @brief Остановить HTTP-сервер.
 */
esp_err_t http_api_stop(void);

#ifdef __cplusplus
}
#endif
