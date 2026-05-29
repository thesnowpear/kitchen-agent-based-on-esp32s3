idf_component_register(
    SRCS "fridge_voice_session.c"
    INCLUDE_DIRS "include"
    REQUIRES audio asr ai_client ai_context ai_actions storage freertos heap log
)
