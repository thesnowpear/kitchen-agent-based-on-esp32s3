idf_component_register(
    SRCS "fridge_speaker.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2s esp_http_client esp-tls nvs_flash network esp_timer heap log
)
