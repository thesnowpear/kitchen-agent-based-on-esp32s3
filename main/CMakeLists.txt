idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash network diagnostics usb_protocol display_test ai_client storage ai_context sensors audio asr
)
