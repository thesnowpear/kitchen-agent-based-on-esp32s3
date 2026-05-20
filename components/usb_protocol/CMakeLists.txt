idf_component_register(
    SRCS "fridge_usb_protocol.c"
    INCLUDE_DIRS "include"
    REQUIRES network diagnostics ai_client freertos
)
