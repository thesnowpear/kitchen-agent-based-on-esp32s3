idf_component_register(
    SRCS "fridge_sensors.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_adc esp_timer freertos log driver radar
)
