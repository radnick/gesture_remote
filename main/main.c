/*
 * main.c — Пульт Pomodoro на жестах (BMI160 + ESP-NOW).
 *
 * Платформа: ESP32-C6, ESP-IDF 6.0.
 *
 * Жесты:
 *   Double-tap → START
 *   Shake      → RESET
 *   Tilt right → TOGGLE_STATE     (следующий режим)
 *   Tilt left  → TOGGLE_STATE_REV (предыдущий режим)
 *
 * Аппаратное подключение BMI160:
 *   SDA  → GPIO6
 *   SCL  → GPIO7
 *   INT1 → GPIO3 (прерывание, активный низкий)
 *   I2C адрес: 0x68 (SDO = GND)
 */

#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bmi160.h"
#include "gesture_engine.h"
#include "espnow_tx.h"

static const char *TAG = "main";

/* ---------- Конфигурация I2C ---------- */
#define I2C_SCL_GPIO            GPIO_NUM_7
#define I2C_SDA_GPIO            GPIO_NUM_6
#define I2C_FREQ_HZ             400000

/*
 * MAC-адрес основного устройства (encoder), КУДА отправляются команды.
 *
 * ВНИМАНИЕ: перед прошивкой замените на реальный MAC вашего ESP32-C6
 *           с проектом encoder.
 *
 * Чтобы узнать MAC основного устройства:
 *   1. Прошейте encoder, откройте монитор:
 *      powershell -ExecutionPolicy Bypass -File "tools\monitor.ps1"
 *   2. Найдите строку вида «Local MAC: aa:bb:cc:dd:ee:ff»
 *   3. Скопируйте байты в массив ниже.
 */
static const uint8_t TARGET_MAC[] = {
    0xA0, 0xA3, 0xB3, 0x00, 0x00, 0x00   /* ЗАМЕНИТЬ на реальный MAC */
};

/* ---------- Точка входа ---------- */

void app_main(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "Gesture Remote starting...");
    ESP_LOGI(TAG, "Target MAC: " MACSTR, MAC2STR(TARGET_MAC));

    /* ----- 1. Инициализация I2C master шины ----- */
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port     = I2C_NUM_0,
        .sda_io_num   = I2C_SDA_GPIO,
        .scl_io_num   = I2C_SCL_GPIO,
        .clk_source   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags        = {
            .enable_internal_pullup = true,
        },
    };

    i2c_master_bus_handle_t i2c_bus = NULL;
    ret = i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "I2C bus initialized: SDA=GPIO%d, SCL=GPIO%d",
             I2C_SDA_GPIO, I2C_SCL_GPIO);

    /* ----- 2. Инициализация BMI160 ----- */
    ret = bmi160_init(i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BMI160 init failed: %s", esp_err_to_name(ret));
        return;
    }

    /* ----- 3. Инициализация движка жестов ----- */
    ret = gesture_engine_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Gesture engine init failed: %s", esp_err_to_name(ret));
        return;
    }

    /* ----- 4. Инициализация ESP-NOW передатчика ----- */
    ret = espnow_tx_init(TARGET_MAC);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW TX init failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "All subsystems initialized. Main loop starting.");

    /* ----- 5. Главный цикл ----- */
    while (1) {
        gesture_cmd_t cmd = gesture_engine_get_cmd();

        if (cmd != GESTURE_CMD_NONE) {
            ESP_LOGI(TAG, "Gesture cmd=%d → sending", (int)cmd);
            esp_err_t tx_ret = espnow_tx_send_cmd(cmd);
            if (tx_ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to send cmd=%d: %s",
                         (int)cmd, esp_err_to_name(tx_ret));
            }
        }

        /* Задержка 20 мс → частота опроса жестов ~50 Гц */
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
