idf_component_register(
    SRCS "fridge_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2s esp_timer freertos heap log
)
