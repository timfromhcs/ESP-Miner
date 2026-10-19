idf_component_register(
SRCS
    "connect.c"
    # Pure decision logic with no ESP-IDF dependency, so the recovery ladder and
    # the fallback re-assert rules can be exercised on the host.
    "net_recovery_policy.c"

INCLUDE_DIRS 
    "include"

REQUIRES
    "asic"
    "nvs_flash"
    "esp_wifi"
    "esp_event"
    "stratum"
    "stratum_v2"
    "espressif__mdns"
    "lwip"
)

target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../main"
    "${CMAKE_CURRENT_LIST_DIR}/../../main/tasks"
)

# Per-device knobs that connect.c itself consumes (see the top of connect.c).
# They are also set for main/, because nvs_config.h reads the same symbols.
#
# Two rules here, both learned the hard way:
#
# 1. A -D is emitted only for a variable that actually has a value. An unset
#    CMake variable expands to nothing, so `-DFOO="${FOO}"` becomes `-DFOO=`,
#    which the preprocessor sees as the *empty string*. That is harmless for the
#    string knobs below, but it silently turns a numeric knob into a string.
#
# 2. ESP_MINER_CONFIG_AP_STUCK is numeric - connect.c compares it against 1 and
#    2 - so it must never be emitted empty and must never be quoted. The test
#    project (test/CMakeLists.txt) defines none of these variables, which is how
#    `"" == 1` reached the compiler there, while the firmware build - which goes
#    through the root CMakeLists and defaults the value to 2 - built fine.
if(NOT DEFINED ESP_MINER_CONFIG_AP_STUCK OR "${ESP_MINER_CONFIG_AP_STUCK}" STREQUAL "")
    set(ESP_MINER_CONFIG_AP_STUCK 0)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ESP_MINER_CONFIG_AP_STUCK=${ESP_MINER_CONFIG_AP_STUCK}
)

foreach(_esp_miner_str ESP_MINER_WIFI_BSSID ESP_MINER_STA_MAC ESP_MINER_DHCP_HOSTNAME)
    if(DEFINED ${_esp_miner_str} AND NOT "${${_esp_miner_str}}" STREQUAL "")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${_esp_miner_str}="${${_esp_miner_str}}")
    endif()
endforeach()
