idf_component_register(
    SRCS "fridge_radar.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_driver_gpio esp_driver_uart esp_timer log
)
