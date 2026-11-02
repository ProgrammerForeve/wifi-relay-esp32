#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Состояние антидребезга, обновляемое по отметкам времени.
 *
 * Чистая функция `button_debounce_step` не трогает GPIO и пригодна
 * для будущих host-based тестов (ADR-0009).
 */
typedef struct {
  bool raw_pressed;
  bool stable_pressed;
  int64_t raw_changed_us;
} button_debounce_t;

/**
 * @brief Настроить GPIO кнопки и запустить задачу опроса.
 *
 * Кнопка не управляет GPIO реле напрямую: короткое и долгое нажатие
 * вызывают интерфейс relay/wifi_mgr (ADR-0007).
 */
esp_err_t button_init(void);

/**
 * @brief Один шаг антидребезга по текущему «сырому» состоянию и времени.
 *
 * Возвращает новое состояние; исходное состояние не изменяется.
 */
button_debounce_t button_debounce_step(button_debounce_t state,
                                       bool raw_pressed, int64_t now_us,
                                       int64_t debounce_us);

#ifdef __cplusplus
}
#endif
