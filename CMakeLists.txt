cmake_minimum_required(VERSION 3.16)

set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/components/gen_bmgr_codes/board_manager.defaults")
    list(APPEND SDKCONFIG_DEFAULTS
        "${CMAKE_CURRENT_LIST_DIR}/components/gen_bmgr_codes/board_manager.defaults")
endif()
list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.bmgr")

set(EXTRA_COMPONENT_DIRS
    "components/signalk_client"
    "components/instrument_ui"
    "components/autopilot_app"
    "components/signalk_auth"
    "components/audio_feedback"
    "components/wifi_manager"
    "components/wifi_settings_app"
    "components/settings_app"
    "components/unit_config"
)

set(EXCLUDE_COMPONENTS "wind_gauge_app" "signalk_app")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# -O2 (CONFIG_COMPILER_OPTIMIZATION_PERF) enables flow analysis that reports intentional, NUL-terminated
# truncations as errors; keep them visible as warnings only.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=stringop-truncation" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=format-truncation" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=maybe-uninitialized" APPEND)

project(waveshare_p4_signalk)
