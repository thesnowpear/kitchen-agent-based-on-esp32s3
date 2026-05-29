idf_component_register(
    SRCS "fridge_mqtt_protocol.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event esp_timer cjson mqtt nvs_flash network diagnostics sensors storage ai_client asr speaker
)
