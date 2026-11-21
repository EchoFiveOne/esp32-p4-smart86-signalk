set(main_requires
    signalk_client
    instruments_app
    autopilot_app
    wifi_settings_app
    wifi_manager
    settings_app
    unit_config
    day_night
    signalk_auth
    audio_feedback
    esp_wifi
    nvs_flash
    esp_event
    mdns
    esp_netif
    lwip
    esp_board_manager
    esp_lvgl_port
    esp_lcd
    espressif__esp_lcd_touch
)

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../components/gen_bmgr_codes/CMakeLists.txt")
    list(APPEND main_requires gen_bmgr_codes)
endif()

idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES ${main_requires}
)
