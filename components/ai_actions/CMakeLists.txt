idf_component_register(
    SRCS "fridge_ai_actions.c"
    INCLUDE_DIRS "include"
    REQUIRES kitchen_tools cjson freertos log
)
