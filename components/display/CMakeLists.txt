idf_component_register(
    SRCS "fridge_display.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_spi freertos heap log nvs_flash
)
