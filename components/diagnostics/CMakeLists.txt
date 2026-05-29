idf_component_register(
    SRCS "fridge_diagnostics.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_system esp_hw_support esp_app_format heap spi_flash esp_timer app_update network
)
