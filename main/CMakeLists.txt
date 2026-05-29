idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash network diagnostics usb_protocol display_test display touch ui ai_client storage ai_context sensors radar audio asr speaker camera wake_word
)
