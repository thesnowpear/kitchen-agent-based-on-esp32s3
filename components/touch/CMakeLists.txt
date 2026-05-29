idf_component_register(
    SRCS "fridge_touch.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio driver freertos log sensors
)
