idf_component_register(
    SRCS "fridge_kitchen_tools.c"
    INCLUDE_DIRS "include"
    REQUIRES cjson esp_timer freertos heap log speaker storage
)
