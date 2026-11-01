#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Настроить GPIO кнопки и запустить задачу опроса.
 *
 * Кнопка не управляет GPIO реле напрямую: короткое и долгое нажатие
 * вызывают интерфейс relay/wifi_mgr (ADR-0007).
 */
esp_err_t button_init(void);

#ifdef __cplusplus
}
#endif
