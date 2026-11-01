#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Логическое состояние реле.
 *
 * Источник истины — переменная под мьютексом, а не чтение пина (ADR-0007).
 */
typedef enum {
    RELAY_STATE_OFF = 0,
    RELAY_STATE_ON,
} relay_state_t;

/**
 * @brief Настроить GPIO реле и привести его в безопасное состояние OFF.
 *
 * Вызывается до остальных подсистем (ADR-0008).
 */
esp_err_t relay_init(void);

/**
 * @brief Прочитать текущее логическое состояние.
 */
relay_state_t relay_get_state(void);

/**
 * @brief Установить состояние и записать его в GPIO под мьютексом.
 */
esp_err_t relay_set_state(relay_state_t state);

/**
 * @brief Переключить реле: чтение — изменение — запись под мьютексом.
 */
esp_err_t relay_toggle(void);

#ifdef __cplusplus
}
#endif
