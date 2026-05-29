idf_component_register(
    SRCS "fridge_camera.c"
    INCLUDE_DIRS "include"
    REQUIRES esp32-camera esp_timer esp_psram heap driver
)
