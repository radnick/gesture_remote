/*
 * bmi160.c — минимальный I2C-драйвер BMI160 для ESP-IDF 6.0.
 *
 * Поддерживает:
 *   - Акселерометр (100 Гц, ±4g)
 *   - Детектор any-motion (встряска) через прерывание INT1
 *   - Детектор double-tap через прерывание INT1
 */

#include "bmi160.h"

#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bmi160";

/* ---------- Адрес I2C ---------- */
#define BMI160_I2C_ADDR         0x68    /* SDO = GND */

/* ---------- Регистры BMI160 ---------- */
#define BMI160_REG_CHIP_ID      0x00    /* R   CHIP_ID, ожидается 0xD1      */
#define BMI160_REG_PMU_STATUS   0x03    /* R   PMU status                   */
#define BMI160_REG_ACC_DATA     0x12    /* R   6 байт: X_L,X_H,Y_L,Y_H,Z_L,Z_H */
#define BMI160_REG_INT_STATUS_0 0x1C    /* R   статус прерываний (0)        */
#define BMI160_REG_INT_STATUS_1 0x1D    /* R   статус прерываний (1)        */
#define BMI160_REG_ACC_CONF     0x40    /* R/W конфигурация акселерометра   */
#define BMI160_REG_ACC_RANGE    0x41    /* R/W диапазон акселерометра       */
#define BMI160_REG_INT_EN_0     0x50    /* R/W разрешение прерываний (0)    */
#define BMI160_REG_INT_EN_1     0x51    /* R/W разрешение прерываний (1)    */
#define BMI160_REG_INT_EN_2     0x52    /* R/W разрешение прерываний (2)    */
#define BMI160_REG_INT_OUT_CTRL 0x53    /* R/W управление выходом INT1/INT2 */
#define BMI160_REG_INT_LATCH    0x54    /* R/W длительность импульса INT    */
#define BMI160_REG_INT_MAP_0    0x55    /* R/W привязка прерываний к INT1   */
#define BMI160_REG_INT_MAP_1    0x56    /* R/W привязка прерываний к INT2   */
#define BMI160_REG_INT_MAP_2    0x57    /* R/W привязка прерываний к INT2   */
#define BMI160_REG_INT_MOTION_0 0x5F    /* R/W any-motion: порог            */
#define BMI160_REG_INT_MOTION_1 0x60    /* R/W any-motion: длительность     */
#define BMI160_REG_INT_MOTION_2 0x61    /* R/W any-motion: конфигурация     */
#define BMI160_REG_INT_MOTION_3 0x62    /* R/W any-motion: конфигурация     */
#define BMI160_REG_INT_TAP_0    0x6B    /* R/W tap quiet                    */
#define BMI160_REG_INT_TAP_1    0x6C    /* R/W tap shock                    */
#define BMI160_REG_INT_TAP_2    0x6D    /* R/W double-tap window            */
#define BMI160_REG_CMD          0x7E    /* W   командный регистр            */

/* ---------- Команды CMD ---------- */
#define BMI160_CMD_SOFTRESET    0xB6
#define BMI160_CMD_ACC_NORMAL   0x11

/* ---------- Битовые маски INT_STATUS_0 ---------- */
#define BMI160_INT_STATUS_ANY_MOTION   (1 << 5)
#define BMI160_INT_STATUS_DOUBLE_TAP   (1 << 4)

/* ---------- Битовые маски INT_EN_0 ---------- */
#define BMI160_INT_EN_ANY_MOT_X  (1 << 0)
#define BMI160_INT_EN_ANY_MOT_Y  (1 << 1)
#define BMI160_INT_EN_ANY_MOT_Z  (1 << 2)
#define BMI160_INT_EN_DOUBLE_TAP (1 << 4)

/* ---------- Битовые маски INT_MAP_0 (INT1) ---------- */
#define BMI160_INT1_ANY_MOTION   (1 << 5)
#define BMI160_INT1_DOUBLE_TAP   (1 << 6)

/* ---------- Битовые маски INT_OUT_CTRL ---------- */
#define BMI160_INT1_OUTPUT_EN    (1 << 3)
#define BMI160_INT1_ACTIVE_LOW   (0 << 1)    /* бит сброшен = active low */
#define BMI160_INT1_PUSHPULL     (0 << 2)    /* бит сброшен = push-pull */

/* ---------- GPIO прерывания ---------- */
#define BMI160_INT1_GPIO         GPIO_NUM_3

/* ---------- Глобальное состояние ---------- */

static i2c_master_dev_handle_t g_dev = NULL;  /* хэндл I2C-устройства       */

/*
 * Единый флаг прерывания: ISR выставляет true.
 * Статус читается из INT_STATUS_0 ОДИН раз при первом обращении
 * любого getter'а. Кэш отслеживает, какие биты ещё не проверены
 * (g_pending_bits), чтобы каждый getter мог независимо проверить
 * свой бит без повторной I2C-транзакции.
 */
static volatile bool g_irq_pending = false;
static uint8_t        g_pending_bits = 0;   /* непроверенные биты статуса */

/* ---------- Низкоуровневые I2C-операции ---------- */

static esp_err_t bmi160_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(g_dev, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

static esp_err_t bmi160_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(g_dev, &reg, 1, data, len,
                                       pdMS_TO_TICKS(100));
}

static esp_err_t bmi160_read_reg(uint8_t reg, uint8_t *val)
{
    return bmi160_read_regs(reg, val, 1);
}

/* ---------- Обработчик прерывания GPIO ---------- */

static void IRAM_ATTR bmi160_int1_isr(void *arg)
{
    (void)arg;
    /*
     * Сигнализируем задаче о факте прерывания.
     * Дизассемблирование причины (tap или any-motion) — в контексте задачи,
     * через чтение INT_STATUS_0, потому что I2C из ISR требует IRAM-
     * совместимого драйвера.
     */
    g_irq_pending = true;
}

/* ---------- Публичные функции ---------- */

esp_err_t bmi160_init(i2c_master_bus_handle_t bus_handle)
{
    esp_err_t ret;
    uint8_t val;

    /* ----- Создать I2C device на шине ----- */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address   = BMI160_I2C_ADDR,
        .scl_speed_hz     = 400000,
    };
    ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &g_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    /* ----- Soft reset ----- */
    ret = bmi160_write_reg(BMI160_REG_CMD, BMI160_CMD_SOFTRESET);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Soft reset failed: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    /* ----- Проверка CHIP_ID ----- */
    ret = bmi160_read_reg(BMI160_REG_CHIP_ID, &val);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CHIP_ID read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (val != 0xD1) {
        ESP_LOGE(TAG, "Unexpected CHIP_ID: 0x%02X (expected 0xD1)", val);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "BMI160 CHIP_ID = 0x%02X — ok", val);

    /* ----- Перевод акселерометра в normal mode ----- */
    ret = bmi160_write_reg(BMI160_REG_CMD, BMI160_CMD_ACC_NORMAL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ACC normal mode failed: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    /* ----- Конфигурация акселерометра ----- */
    /* ACC_CONF: ODR=100Hz (0x08 в старшем ниббле), BW=normal (0x02 в младшем) */
    ret = bmi160_write_reg(BMI160_REG_ACC_CONF, 0x82);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ACC_CONF write failed: %s", esp_err_to_name(ret));
        return ret;
    }
    /* ACC_RANGE: ±4g */
    ret = bmi160_write_reg(BMI160_REG_ACC_RANGE, 0x05);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ACC_RANGE write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ----- Настройка any-motion (встряска) ----- */
    /* Порог: 0x14 = 20 counts (≈80 mg при ±4g, шаг 3.91 mg/count) */
    ret = bmi160_write_reg(BMI160_REG_INT_MOTION_0, 0x14);
    if (ret != ESP_OK) goto int_cfg_fail;
    /* Длительность: 2 отсчёта (20 мс при 100 Гц) */
    ret = bmi160_write_reg(BMI160_REG_INT_MOTION_1, 0x02);
    if (ret != ESP_OK) goto int_cfg_fail;
    /* Включить any-motion (любая ось) */
    ret = bmi160_write_reg(BMI160_REG_INT_MOTION_2, 0x07);
    if (ret != ESP_OK) goto int_cfg_fail;

    /* ----- Настройка double-tap ----- */
    /* INT_TAP_0: tap quiet = 0x06 (60 мс при 100 Гц ODR, шаг 10 мс) */
    ret = bmi160_write_reg(BMI160_REG_INT_TAP_0, 0x06);
    if (ret != ESP_OK) goto int_cfg_fail;
    /* INT_TAP_1: tap shock = 0x06 (60 мс) */
    ret = bmi160_write_reg(BMI160_REG_INT_TAP_1, 0x06);
    if (ret != ESP_OK) goto int_cfg_fail;
    /* INT_TAP_2: double-tap window = 0x0A (100 мс, шаг 10 мс) */
    ret = bmi160_write_reg(BMI160_REG_INT_TAP_2, 0x0A);
    if (ret != ESP_OK) goto int_cfg_fail;

    /* ----- Привязка прерываний к INT1 ----- */
    /* INT_MAP_0: any_motion → INT1 (bit5), double_tap → INT1 (bit6) */
    ret = bmi160_write_reg(BMI160_REG_INT_MAP_0,
                           BMI160_INT1_ANY_MOTION | BMI160_INT1_DOUBLE_TAP);
    if (ret != ESP_OK) goto int_cfg_fail;

    /* ----- Разрешение прерываний ----- */
    /* INT_EN_0: any_motion X/Y/Z + double_tap */
    ret = bmi160_write_reg(BMI160_REG_INT_EN_0,
                           BMI160_INT_EN_ANY_MOT_X |
                           BMI160_INT_EN_ANY_MOT_Y |
                           BMI160_INT_EN_ANY_MOT_Z |
                           BMI160_INT_EN_DOUBLE_TAP);
    if (ret != ESP_OK) goto int_cfg_fail;

    /* ----- Конфигурация выхода INT1 ----- */
    /* INT_LATCH = 0 (non-latched: флаг держится до чтения статуса) */
    ret = bmi160_write_reg(BMI160_REG_INT_LATCH, 0x00);
    if (ret != ESP_OK) goto int_cfg_fail;

    /* INT_OUT_CTRL: INT1 output enable, active low, push-pull */
    ret = bmi160_write_reg(BMI160_REG_INT_OUT_CTRL,
                           BMI160_INT1_OUTPUT_EN |
                           BMI160_INT1_ACTIVE_LOW |
                           BMI160_INT1_PUSHPULL);
    if (ret != ESP_OK) goto int_cfg_fail;

    /* ----- Настройка GPIO3 как входа с pull-up ----- */
    gpio_config_t io_cfg = {
        .intr_type    = GPIO_INTR_NEGEDGE,  /* INT1 active low → falling edge */
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = BIT64(BMI160_INT1_GPIO),
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ret = gpio_config(&io_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config INT1 failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Установка ISR-сервиса GPIO (если ещё не установлен) */
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    /* Регистрация обработчика прерывания */
    ret = gpio_isr_handler_add(BMI160_INT1_GPIO, bmi160_int1_isr, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "BMI160 initialized: ACC 100Hz ±4g, INT1=GPIO%d", BMI160_INT1_GPIO);
    return ESP_OK;

int_cfg_fail:
    ESP_LOGE(TAG, "Interrupt config register write failed: %s",
             esp_err_to_name(ret));
    return ret;
}

esp_err_t bmi160_read_accel(bmi160_accel_t *out)
{
    uint8_t buf[6];
    esp_err_t ret;

    if (g_dev == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = bmi160_read_regs(BMI160_REG_ACC_DATA, buf, sizeof(buf));
    if (ret != ESP_OK) {
        return ret;
    }

    /* Данные в little-endian: LSB, затем MSB */
    out->x = (int16_t)((buf[1] << 8) | buf[0]);
    out->y = (int16_t)((buf[3] << 8) | buf[2]);
    out->z = (int16_t)((buf[5] << 8) | buf[4]);

    return ESP_OK;
}

/*
 * Внутренняя функция: при первом вызове после ISR читает INT_STATUS_0
 * и сохраняет значимые биты в g_pending_bits. Последующие вызовы
 * используют кэш.
 */
static void bmi160_fetch_status(void)
{
    if (g_pending_bits != 0) {
        return;   /* кэш ещё не исчерпан */
    }
    if (!g_irq_pending) {
        return;   /* новых прерываний не было */
    }

    g_irq_pending = false;
    uint8_t status = 0;
    if (bmi160_read_reg(BMI160_REG_INT_STATUS_0, &status) == ESP_OK) {
        /* Сохраняем только интересующие нас биты */
        g_pending_bits = status & (BMI160_INT_STATUS_ANY_MOTION |
                                   BMI160_INT_STATUS_DOUBLE_TAP);
    }
    /* При ошибке I2C: g_pending_bits останется 0, попробуем в следующий раз */
}

bool bmi160_get_double_tap(void)
{
    bmi160_fetch_status();

    if (g_pending_bits & BMI160_INT_STATUS_DOUBLE_TAP) {
        g_pending_bits &= ~BMI160_INT_STATUS_DOUBLE_TAP;
        ESP_LOGI(TAG, "Double-tap detected");
        return true;
    }
    return false;
}

bool bmi160_get_shake(void)
{
    bmi160_fetch_status();

    if (g_pending_bits & BMI160_INT_STATUS_ANY_MOTION) {
        g_pending_bits &= ~BMI160_INT_STATUS_ANY_MOTION;
        ESP_LOGI(TAG, "Shake (any-motion) detected");
        return true;
    }
    return false;
}
