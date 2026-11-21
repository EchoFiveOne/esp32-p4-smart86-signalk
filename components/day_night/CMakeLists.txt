idf_component_register(
    SRCS
        "day_night.cpp"
        "day_night_tz.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        signalk_client
        unit_config
        lvgl__lvgl
        esp_timer
        nvs_flash
        log
)
