#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "relay.h"

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

/**
 * @brief Разобрать тело PUT /api/relay и получить целевое состояние.
 *
 * Чистая функция без побочных эффектов (ADR-0009): принимает только
 * `{"state":"on"}` или `{"state":"off"}` с допустимыми пробелами.
 * Возвращает false при любой ошибке формата.
 */
bool http_api_parse_relay_state(const char *body, size_t len,
                                relay_state_t *state);

/**
 * @brief Разобрать form-urlencoded тело POST /api/provision.
 *
 * Чистая функция без побочных эффектов (ADR-0009): извлекает поля
 * `ssid` и `password`, декодирует `%XX` и `+`, отвергает пустой SSID,
 * NUL-байты и слишком длинные значения. Пароль может быть пустым.
 */
bool http_api_parse_provision_form(const char *body, size_t len, char *ssid,
                                   size_t ssid_size, char *password,
                                   size_t password_size);

#ifdef __cplusplus
}
#endif
