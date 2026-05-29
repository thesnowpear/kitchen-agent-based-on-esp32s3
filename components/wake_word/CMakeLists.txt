idf_component_register(
    SRCS "fridge_wake_word.c"
    INCLUDE_DIRS "include"
    REQUIRES audio speaker esp_timer freertos heap log
    PRIV_REQUIRES espressif__esp-sr
)
