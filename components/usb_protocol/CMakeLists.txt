idf_component_register(
    SRCS "fridge_usb_protocol.c"
    INCLUDE_DIRS "include"
    REQUIRES network diagnostics mqtt_protocol ai_client ai_context storage sensors radar audio asr speaker camera wake_word touch freertos
)
