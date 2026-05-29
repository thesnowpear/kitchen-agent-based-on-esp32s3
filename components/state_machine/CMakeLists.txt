idf_component_register(
    SRCS "fridge_state_machine.c"
    INCLUDE_DIRS "include"
    REQUIRES sensors radar network audio voice_session freertos heap log nvs_flash esp_timer
)
