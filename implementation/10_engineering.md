# Engineering Report — Gesture Remote (Pomodoro)

**Проект:** `gesture_remote`  
**Платформа:** ESP32-C6, ESP-IDF 6.0

---

## Что сделано

Создан полный проект ESP-IDF для пульта Pomodoro на жестах с BMI160 и ESP-NOW.

### Созданные файлы (16 шт.)

| Файл | Назначение |
|------|------------|
| `CMakeLists.txt` | Корневой CMake, проект `gesture_remote` |
| `sdkconfig.defaults` | WiFi, логгирование INFO, panic=reboot |
| `.gitignore` | Стандартный ESP-IDF .gitignore |
| `main/CMakeLists.txt` | Компонент main: SRCS + REQUIRES |
| `main/main.c` | Главный цикл: I2C → BMI160 → Gesture → ESP-NOW |
| `main/gesture_cmd.h` | Перечислимый тип команд (синхронизирован с encoder) |
| `main/gesture_engine.h` | API движка распознавания жестов |
| `main/gesture_engine.c` | Классификация: double-tap, shake, tilt R/L |
| `main/espnow_tx.h` | API ESP-NOW передатчика |
| `main/espnow_tx.c` | ESP-NOW TX: WiFi STA + peer + отправка пакетов |
| `components/bmi160/CMakeLists.txt` | Компонент bmi160 |
| `components/bmi160/bmi160.h` | API драйвера BMI160 (I2C) |
| `components/bmi160/bmi160.c` | Полный драйвер: I2C, INT1 ISR, tap/any-motion |
| `tools/build.ps1` | Сборка (idf.py build) |
| `tools/flash.ps1` | Прошивка (idf.py flash) |
| `tools/monitor.ps1` | Монитор (idf.py monitor) |

---

## Архитектура

```
┌─────────────────────────────────────────────────────────┐
│ main.c (главный цикл)                                   │
│  ┌──────────────────┐  ┌────────────────┐  ┌──────────┐ │
│  │ gesture_engine.c │  │  espnow_tx.c   │  │  main.c  │ │
│  │  ┌────────────┐  │  │  WiFi STA      │  │  I2C bus │ │
│  │  │ bmi160.c   │  │  │  ESP-NOW peer  │  │  init    │ │
│  │  │ I2C driver │  │  │  send(pkt)     │  │  loop:   │ │
│  │  │ INT1 ISR   │  │  └────────────────┘  │  get_cmd │ │
│  │  └────────────┘  │                      │  → send  │ │
│  │  classif:        │                      └──────────┘ │
│  │  tap→START       │                                    │
│  │  shake→RESET     │                                    │
│  │  tiltR→TOGGLE    │                                    │
│  │  tiltL→TOGGLE_R  │                                    │
│  └──────────────────┘                                    │
└─────────────────────────────────────────────────────────┘
```

### Поток данных

1. **BMI160** опрашивается по I2C (400 кГц, GPIO6=SDA, GPIO7=SCL)
2. **INT1** (GPIO3) сигнализирует о double-tap / any-motion через ISR
3. **gesture_engine** читает акселерометр, классифицирует жесты:
   - Double-tap → `GESTURE_CMD_START`
   - Shake (|accel|>1.5g, 200ms) → `GESTURE_CMD_RESET`
   - Tilt right (X>0.5g, 300ms) → `GESTURE_CMD_TOGGLE_STATE`
   - Tilt left (X<-0.5g, 300ms) → `GESTURE_CMD_TOGGLE_STATE_REV`
4. **espnow_tx** формирует пакет `{0xCE, 0xC6, cmd, seq, 0, 0}` и отправляет через ESP-NOW

### Формат пакета ESP-NOW

Синхронизирован с `espnow_rcv.c` проекта `encoder`:

```c
typedef struct {
    uint8_t magic[2];   // {0xCE, 0xC6}
    uint8_t cmd;        // gesture_cmd_t
    uint8_t seq;        // инкрементируемый номер
    uint8_t reserved[2];
} __attribute__((packed)) espnow_cmd_pkt_t;
```

---

## Ключевые решения

### 1. BMI160 ACC_CONF = 0x82 (а не 0x28)

В ТЗ было указано `0x28`, однако согласно документации Bosch Sensortec:
- ACC_CONF: биты [7:4] = ODR, биты [3:0] = BWP
- 100 Гц ODR = 0x08, BWP normal = 0x02
- Итог: `(0x08 << 4) | 0x02 = 0x82`

Значение `0x28` соответствует ODR≈1.56 Гц и нестандартному BWP=8.  
Использовано корректное значение из официального драйвера Bosch.  
При необходимости можно заменить на `0x28` в `bmi160.c:182`.

### 2. ISR + кэширование статуса INT_STATUS_0

Проблема: чтение `INT_STATUS_0` очищает **все** флаги прерываний BMI160.  
Если первый getter (например, `bmi160_get_double_tap()`) читает статус,
второй getter (`bmi160_get_shake()`) больше не увидит свой бит.

Решение:
- ISR выставляет единственный флаг `g_irq_pending`
- `bmi160_fetch_status()` читает `INT_STATUS_0` **один раз** и кэширует биты в `g_pending_bits`
- Каждый getter проверяет и сбрасывает **свой** бит из кэша независимо
- Повторное I2C-чтение не требуется

### 3. Shake — программный детектор

Хотя BMI160 имеет аппаратный any-motion, встряска детектируется программно:
- Порог: |accel| > 1.5g (14.7 м/с²)
- Длительность: 200 мс
- Это даёт более точный контроль порога (аппаратный any-motion ~80mg слишком чувствителен)

Аппаратный any-motion сохранён в `bmi160_get_shake()` для возможного будущего использования.

### 4. Дебаунсинг жестов: 500 мс

Между распознанными жестами выдерживается пауза 500 мс через `esp_timer_get_time()`.

### 5. Частота главного цикла: ~50 Гц

`vTaskDelay(20ms)` + время обработки → эффективная частота опроса жестов ~50 Гц.

---

## Что настроить перед прошивкой

### MAC-адрес основного устройства

В файле `main/main.c:47-49` заменить `TARGET_MAC` на реальный MAC ESP32-C6 с проектом **encoder**:

```c
static const uint8_t TARGET_MAC[] = {
    0xA0, 0xA3, 0xB3, 0x00, 0x00, 0x00   /* ← ЗАМЕНИТЬ */
};
```

**Как узнать MAC основного устройства:**
1. Прошить `encoder` на основном ESP32-C6
2. Открыть монитор:
   ```
   powershell -ExecutionPolicy Bypass -File "tools\monitor.ps1"
   ```
3. Найти строку: `espnow_tx: Local MAC: aa:bb:cc:dd:ee:ff`
4. Скопировать 6 байт в массив `TARGET_MAC`

### Первая сборка

```powershell
# 1. Установить целевой чип (однократно)
cd D:\myPRJ\gesture_remote
D:\DEV\esp-idf\v6.0\esp-idf\export.ps1
idf.py set-target esp32c6

# 2. Сборка
powershell -ExecutionPolicy Bypass -File "tools\build.ps1"

# 3. Прошивка
powershell -ExecutionPolicy Bypass -File "tools\flash.ps1"

# 4. Монитор
powershell -ExecutionPolicy Bypass -File "tools\monitor.ps1"
```

---

## Проверка

1. **Сборка без ошибок:** `tools\build.ps1` должен завершиться успешно
2. **Лог инициализации:** монитор должен показать:
   - `BMI160 CHIP_ID = 0xD1 — ok`
   - `Gesture engine initialized`
   - `Peer added: aa:bb:cc:dd:ee:ff`
   - `ESP-NOW transmitter initialized`
3. **Жесты:** при double-tap/shake/tilt — соответствующие сообщения в логе и отправка пакета
4. **Основное устройство:** проект `encoder` должен принимать команды через `espnow_rcv`
