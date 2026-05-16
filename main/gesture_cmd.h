#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Перечислимый тип команд Pomodoro.
 * Синхронизируется вручную с pomodoro_cmd_t из проекта encoder.
 * Числовые значения важны — они передаются по ESP-NOW.
 */
typedef enum {
    GESTURE_CMD_NONE = 0,
    GESTURE_CMD_START,
    GESTURE_CMD_PAUSE,
    GESTURE_CMD_RESUME,
    GESTURE_CMD_RESET,
    GESTURE_CMD_TOGGLE_STATE,
    GESTURE_CMD_TOGGLE_STATE_REV,
    GESTURE_CMD_SETTINGS,
    GESTURE_CMD_SETTINGS_NEXT,
    GESTURE_CMD_SETTINGS_PREV,
    GESTURE_CMD_SETTINGS_SELECT,
    GESTURE_CMD_SETTINGS_CHANGE_UP,
    GESTURE_CMD_SETTINGS_CHANGE_DOWN,
    GESTURE_CMD_SETTINGS_BACK,
    GESTURE_CMD_SETTINGS_SAVE
} gesture_cmd_t;

#ifdef __cplusplus
}
#endif
