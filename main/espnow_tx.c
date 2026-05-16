/*
 * espnow_tx.c — передатчик команд Pomodoro через ESP-NOW.
 *
 * Формат пакета синхронизирован с espnow_rcv.c проекта encoder:
 *   magic[2] = { 0xCE, 0xC6 }
 *   cmd       = gesture_cmd_t (1 байт)
 *   seq       = инкрементируемый номер (для dedup)
 *   reserved[2]
 */

#include "espnow_tx.h"

#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "espnow_tx";

/* ---------- Константы пакета (синхронизированы с espnow_rcv.c) ---------- */

#define ESPNOW_MAGIC_0  0xCE
#define ESPNOW_MAGIC_1  0xC6
#define ESPNOW_PKT_LEN  6

/* ---------- Структура пакета (должна совпадать с espnow_rcv.c) ---------- */

typedef struct {
    uint8_t magic[2];    /* 0xCE, 0xC6 — сигнатура */
    uint8_t cmd;         /* gesture_cmd_t (1 байт)  */
    uint8_t seq;         /* номер пакета            */
    uint8_t reserved[2];
} __attribute__((packed)) espnow_cmd_pkt_t;

/* ---------- Глобальное состояние ---------- */

static bool     g_initialized = false;
static uint8_t  g_seq = 0;

/* ---------- Callback отправки (не обязателен, но полезен для логов) ---------- */

static void on_send(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    if (status == ESP_NOW_SEND_SUCCESS) {
        ESP_LOGD(TAG, "Packet sent to " MACSTR, MAC2STR(mac_addr));
    } else {
        ESP_LOGW(TAG, "Send failed to " MACSTR, MAC2STR(mac_addr));
    }
}

/* ---------- Публичные функции ---------- */

esp_err_t espnow_tx_init(const uint8_t *target_mac)
{
    esp_err_t ret;

    if (target_mac == NULL) {
        ESP_LOGE(TAG, "target_mac is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    /* ----- Инициализация NVS (нужно для WiFi) ----- */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ----- Инициализация сетевого стека ----- */
    ret = esp_netif_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    esp_netif_create_default_wifi_sta();

    /* ----- Инициализация WiFi STA ----- */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "WiFi STA started");

    /* ----- Инициализация ESP-NOW ----- */
    ret = esp_now_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_now_register_send_cb(on_send);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_register_send_cb failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    /* ----- Добавление peer ----- */
    esp_now_peer_info_t peer = {
        .channel = 0,
        .ifidx   = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, target_mac, 6);

    ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_add_peer failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Peer added: " MACSTR, MAC2STR(target_mac));

    /* ----- Получить и вывести собственный MAC ----- */
    uint8_t local_mac[6];
    ret = esp_read_mac(local_mac, ESP_MAC_WIFI_STA);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Local MAC: " MACSTR, MAC2STR(local_mac));
    }

    g_seq = 0;
    g_initialized = true;
    ESP_LOGI(TAG, "ESP-NOW transmitter initialized");
    return ESP_OK;
}

esp_err_t espnow_tx_send_cmd(gesture_cmd_t cmd)
{
    if (!g_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Формирование пакета */
    espnow_cmd_pkt_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic[0] = ESPNOW_MAGIC_0;
    pkt.magic[1] = ESPNOW_MAGIC_1;
    pkt.cmd      = (uint8_t)cmd;
    pkt.seq      = g_seq++;

    ESP_LOGI(TAG, "Sending cmd=%d seq=%d", pkt.cmd, pkt.seq);

    esp_err_t ret = esp_now_send(NULL, (const uint8_t *)&pkt, ESPNOW_PKT_LEN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_send failed: %s", esp_err_to_name(ret));
    }
    return ret;
}
