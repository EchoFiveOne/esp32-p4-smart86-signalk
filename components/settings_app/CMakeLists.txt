idf_component_register(
    SRCS "settings_app.cpp" "settings_gear_icon.c"
    INCLUDE_DIRS "."
    REQUIRES
        espressif__esp-ui
        unit_config
        day_night
        log
        esp_timer
)
