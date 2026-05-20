idf_component_register(
    SRCS "fridge_network.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif nvs_flash lwip
)
