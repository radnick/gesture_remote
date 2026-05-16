#pragma once
#include "gesture_cmd.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Инициализировать ESP-NOW передатчик.
 *
 * Инициализирует WiFi в режиме STA и ESP-NOW.
 * Добавляет целевое устройство как peer.
 *
 * @param target_mac MAC-адрес основного устройства (6 байт).
 * @return ESP_OK при успехе, иначе код ошибки.
 */
esp_err_t espnow_tx_init(const uint8_t *target_mac);

/**
 * @brief Отправить команду Pomodoro на основное устройство.
 *
 * Формирует пакет с magic-байтами, командой и номером последовательности.
 *
 * @param cmd команда из gesture_cmd_t.
 * @return ESP_OK при успехе, иначе код ошибки.
 */
esp_err_t espnow_tx_send_cmd(gesture_cmd_t cmd);

#ifdef __cplusplus
}
#endif
