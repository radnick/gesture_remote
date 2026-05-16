idf_component_register(
    SRCS "main.c" "gesture_engine.c" "espnow_tx.c"
    INCLUDE_DIRS "."
    REQUIRES esp_system log esp_timer bmi160
)
