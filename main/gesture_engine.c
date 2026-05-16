/*
 * gesture_engine.c — движок распознавания жестов на основе BMI160.
 *
 * Классифицирует жесты:
 *   - Double-tap  (аппаратный детектор BMI160) → GESTURE_CMD_START
 *   - Shake       (программный порог |accel| > 1.5g, 200 мс) → GESTURE_CMD_RESET
 *   - Tilt right  (X > 0.5g, 300 мс) → GESTURE_CMD_TOGGLE_STATE
 *   - Tilt left   (X < -0.5g, 300 мс) → GESTURE_CMD_TOGGLE_STATE_REV
 *
 * Дебаунсинг: между жестами не менее 500 мс.
 */

#include "gesture_engine.h"
#include "bmi160.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <math.h>

static const char *TAG = "gesture";

/* ---------- Константы ---------- */

/* Порог встряски: 1.5g. При ±4g диапазоне: 1 LSB ≈ 0.001196 м/с².
 * 1.5g = 1.5 * 9.8 = 14.7 м/с²
 * В LSB: floor(14.7 / 0.001196) ≈ 12290 */
#define SHAKE_THRESHOLD_G       1.5f
#define SHAKE_DURATION_US       200000UL    /* 200 мс */

/* Порог наклона: 0.5g.
 * 0.5g = 4.9 м/с². В LSB: floor(4.9 / 0.001196) ≈ 4097 */
#define TILT_THRESHOLD_G        0.5f
#define TILT_DURATION_US        300000UL    /* 300 мс */

/* Дебаунсинг между жестами */
#define GESTURE_DEBOUNCE_US     500000UL    /* 500 мс */

/* Ускорение свободного падения (м/с²) */
#define GRAVITY_MS2             9.8f

/*
 * Коэффициент перевода LSB → м/с² при диапазоне ±4g.
 *   range_g  = 4.0
 *   max_lsb  = 32768
 *   ms2_per_lsb = range_g * GRAVITY_MS2 / max_lsb
 */
#define ACCEL_RANGE_G           4.0f
#define ACCEL_MAX_LSB           32768.0f
#define MS2_PER_LSB             ((ACCEL_RANGE_G * GRAVITY_MS2) / ACCEL_MAX_LSB)

/* ---------- Глобальное состояние ---------- */

static int64_t g_last_gesture_us = 0;          /* время последнего жеста   */

/* Состояние детектора встряски */
static int64_t g_shake_start_us = 0;
static bool    g_shake_active = false;

/* Состояние детектора наклона */
static int64_t g_tilt_start_us = 0;
static bool    g_tilt_active = false;
static int     g_tilt_direction = 0;           /* +1 = right, -1 = left    */

/* ---------- Вспомогательные функции ---------- */

static inline float accel_magnitude_ms2(const bmi160_accel_t *a)
{
    float x = (float)a->x * MS2_PER_LSB;
    float y = (float)a->y * MS2_PER_LSB;
    float z = (float)a->z * MS2_PER_LSB;
    return sqrtf(x * x + y * y + z * z);
}

static inline int64_t now_us(void)
{
    return esp_timer_get_time();
}

/* Проверка дебаунсинга */
static bool debounce_ok(void)
{
    return (now_us() - g_last_gesture_us) >= GESTURE_DEBOUNCE_US;
}

/* Сброс всех детекторов */
static void reset_detectors(void)
{
    g_shake_active = false;
    g_shake_start_us = 0;
    g_tilt_active = false;
    g_tilt_start_us = 0;
    g_tilt_direction = 0;
}

/* Зафиксировать жест (сброс детекторов и обновление таймера) */
static void commit_gesture(void)
{
    g_last_gesture_us = now_us();
    reset_detectors();
}

/* ---------- Публичные функции ---------- */

esp_err_t gesture_engine_init(void)
{
    g_last_gesture_us = 0;
    reset_detectors();
    ESP_LOGI(TAG, "Gesture engine initialized");
    return ESP_OK;
}

gesture_cmd_t gesture_engine_get_cmd(void)
{
    gesture_cmd_t result = GESTURE_CMD_NONE;

    /* ----- 1. Проверка аппаратных событий BMI160 ----- */

    /* Double-tap (аппаратный детектор через INT1) */
    if (bmi160_get_double_tap()) {
        if (debounce_ok()) {
            ESP_LOGI(TAG, "Gesture: DOUBLE_TAP → START");
            commit_gesture();
            return GESTURE_CMD_START;
        } else {
            ESP_LOGD(TAG, "Double-tap ignored (debounce)");
        }
    }

    /* ----- 2. Чтение акселерометра ----- */

    bmi160_accel_t accel;
    if (bmi160_read_accel(&accel) != ESP_OK) {
        /* Ошибка чтения — возвращаем NONE */
        return GESTURE_CMD_NONE;
    }

    float mag = accel_magnitude_ms2(&accel);
    float accel_x_ms2 = (float)accel.x * MS2_PER_LSB;

    /* ----- 3. Детектор встряски (Shake) ----- */

    float shake_threshold_ms2 = SHAKE_THRESHOLD_G * GRAVITY_MS2;  /* ≈14.7 */

    if (mag > shake_threshold_ms2) {
        if (!g_shake_active) {
            g_shake_active = true;
            g_shake_start_us = now_us();
        } else if ((now_us() - g_shake_start_us) >= SHAKE_DURATION_US) {
            if (debounce_ok()) {
                ESP_LOGI(TAG, "Gesture: SHAKE (mag=%.2f) → RESET", (double)mag);
                commit_gesture();
                return GESTURE_CMD_RESET;
            } else {
                reset_detectors();
            }
        }
        /* Shake активен — не проверяем tilt в этом же кадре */
        return GESTURE_CMD_NONE;
    } else {
        /* Ускорение ниже порога — сбрасываем детектор встряски */
        if (g_shake_active) {
            g_shake_active = false;
            g_shake_start_us = 0;
        }
    }

    /* ----- 4. Детектор наклона (Tilt) ----- */

    float tilt_threshold_ms2 = TILT_THRESHOLD_G * GRAVITY_MS2;    /* ≈4.9 */
    int current_direction = 0;

    if (accel_x_ms2 > tilt_threshold_ms2) {
        current_direction = 1;   /* наклон вправо */
    } else if (accel_x_ms2 < -tilt_threshold_ms2) {
        current_direction = -1;  /* наклон влево */
    }

    if (current_direction != 0) {
        if (!g_tilt_active || g_tilt_direction != current_direction) {
            /* Начало нового наклона или смена направления */
            g_tilt_active = true;
            g_tilt_direction = current_direction;
            g_tilt_start_us = now_us();
        } else if ((now_us() - g_tilt_start_us) >= TILT_DURATION_US) {
            /* Наклон удерживается достаточное время */
            if (debounce_ok()) {
                if (g_tilt_direction > 0) {
                    ESP_LOGI(TAG, "Gesture: TILT_RIGHT (x=%.2f) → TOGGLE_STATE",
                             (double)accel_x_ms2);
                    commit_gesture();
                    return GESTURE_CMD_TOGGLE_STATE;
                } else {
                    ESP_LOGI(TAG, "Gesture: TILT_LEFT (x=%.2f) → TOGGLE_STATE_REV",
                             (double)accel_x_ms2);
                    commit_gesture();
                    return GESTURE_CMD_TOGGLE_STATE_REV;
                }
            } else {
                reset_detectors();
            }
        }
    } else {
        /* Ускорение по X в мёртвой зоне — сбрасываем детектор наклона */
        if (g_tilt_active) {
            g_tilt_active = false;
            g_tilt_start_us = 0;
            g_tilt_direction = 0;
        }
    }

    return GESTURE_CMD_NONE;
}
