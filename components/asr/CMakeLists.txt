idf_component_register(
    SRCS "fridge_asr.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp-tls nvs_flash audio network esp_timer heap log cjson
)
