idf_component_register(
    SRCS "fridge_ai_client.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp_timer esp-tls network nvs_flash
)
