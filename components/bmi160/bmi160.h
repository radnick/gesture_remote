#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Структура данных акселерометра: сырые отсчёты (LSB). */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
} bmi160_accel_t;

/**
 * @brief Инициализировать BMI160 по I2C.
 *
 * Выполняет soft-reset, проверяет CHIP_ID, настраивает:
 *   - Акселерометр: 100 Гц, normal BW, ±4g
 *   - Детектор any-motion (встряска)
 *   - Детектор double-tap
 *   - Прерывание на INT1 (GPIO3), активный низкий уровень
 *
 * @param bus_handle хэндл I2C master шины.
 * @return ESP_OK при успехе.
 */
esp_err_t bmi160_init(i2c_master_bus_handle_t bus_handle);

/**
 * @brief Прочитать текущие данные акселерометра.
 *
 * @param out указатель на структуру для сырых значений.
 * @return ESP_OK при успехе.
 */
esp_err_t bmi160_read_accel(bmi160_accel_t *out);

/**
 * @brief Проверить, был ли double-tap (сбрасывает флаг после чтения).
 *
 * @return true если double-tap обнаружен.
 */
bool bmi160_get_double_tap(void);

/**
 * @brief Проверить, была ли встряска (сбрасывает флаг после чтения).
 *
 * @return true если any-motion обнаружен.
 */
bool bmi160_get_shake(void);

#ifdef __cplusplus
}
#endif
