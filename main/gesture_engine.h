#pragma once
#include "gesture_cmd.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Типы распознаваемых жестов BMI160 */
typedef enum {
    GESTURE_NONE = 0,
    GESTURE_DOUBLE_TAP,
    GESTURE_SHAKE,
    GESTURE_TILT_RIGHT,
    GESTURE_TILT_LEFT,
} gesture_type_t;

/**
 * @brief Инициализировать движок распознавания жестов.
 *
 * Должен вызываться после bmi160_init().
 *
 * @return ESP_OK при успехе.
 */
esp_err_t gesture_engine_init(void);

/**
 * @brief Получить следующую команду Pomodoro (неблокирующий вызов).
 *
 * Движок самостоятельно опрашивает BMI160 и классифицирует жесты.
 * Применяется дебаунсинг: между жестами не менее 500 мс.
 *
 * @return gesture_cmd_t. GESTURE_CMD_NONE если жестов нет.
 */
gesture_cmd_t gesture_engine_get_cmd(void);

#ifdef __cplusplus
}
#endif
