option(FRIDGE_RECOVERY_BUILD "Build the small ota_1 local recovery firmware profile" OFF)

set(fridge_recovery_requires
    nvs_flash
    app_update
    esp_partition
    esp_https_ota
    esp_http_client
    mbedtls
    network
    diagnostics
)

if(FRIDGE_RECOVERY_BUILD OR CONFIG_FRIDGE_RECOVERY_APP)
    set(fridge_main_requires ${fridge_recovery_requires})
else()
    set(fridge_main_requires
        ${fridge_recovery_requires}
        usb_protocol
        display_test
        display
        touch
        ui
        ai_client
        storage
        ai_context
        ai_actions
        sensors
        radar
        state_machine
        audio
        asr
        voice_session
        speaker
        kitchen_tools
        camera
        wake_word
    )
endif()

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES ${fridge_main_requires}
)
