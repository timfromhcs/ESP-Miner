idf_component_register(
SRCS
    "adc.c"
    "i2c_bitaxe.c"
    "main.c"
    "log_buffer.c"
    "nvs_config.c"
    "display.c"
    "screen.c"
    "input.c"
    "filesystem.c"
    "system.c"
    "work_queue.c"
    "memory_guard.c"
    "lv_font_portfolio-6x8.c"
    "logo.c"
    "images/generated/bitaxe_background.c"
    "images/generated/bitaxe_logo_large.c"
    "images/fonts/ui_font_DigitalNumbers16.c"
    "images/fonts/ui_font_DigitalNumbers28.c"
    "images/fonts/ui_font_OpenSansBold13.c"
    "images/fonts/ui_font_OpenSansBold14.c"
    "images/fonts/ui_font_AngelWish40.c"
    "./bap/bap.c"
    "./bap/bap_protocol.c"
    "./bap/bap_uart.c"
    "./bap/bap_handlers.c"
    "./bap/bap_subscription.c"
    "device_pins.c"
    "device_config.c"
    "setup_ble.c"
    "task_monitor.c"
    "./http_server/http_server.c"
    "./http_server/websocket.c"
    "./http_server/websocket_log.c"
    "./http_server/websocket_api.c"
    "./http_server/cjson_utils.c"
    "./http_server/system_api_json.c"
    "./http_server/theme_api.c"
    "./http_server/axe-os/api/system/asic_settings.c"
    "./self_test/self_test.c"
    "./tasks/stratum_v1_task.c"
    "./tasks/stratum_v2_task.c"
    "./tasks/protocol_coordinator.c"
    "./tasks/create_jobs_task.c"
    "./tasks/asic_result_task.c"
    "./tasks/power_management_task.c"
    "./tasks/statistics_task.c"
    "./tasks/scoreboard.c"
    "./tasks/hashrate_monitor_task.c"
    "./tasks/fan_controller_task.c"
    "./thermal/EMC2101.c"
    "./thermal/EMC2103.c"
    "./thermal/EMC2302.c"
    "./thermal/TMP1075.c"
    "./thermal/thermal.c"
    "./thermal/PID.c"
    "./thermal/atm_policy.c"
    "./power/TPS546.c"
    "./power/DS4432U.c"
    "./power/INA260.c"
    "./power/power.c"
    "./power/vcore.c"
    "./power/vf_tuner.c"
    "./power/power_target.c"
    "./power/asic_reset.c"
    "./power/asic_init.c"
    "${CMAKE_CURRENT_BINARY_DIR}/embedded_web_ui.c"

INCLUDE_DIRS
    "."
    "tasks"
    "http_server"
    "http_server/axe-os/api/system"
    "self_test"
    "bap"
    "thermal"
    "power"
    "${CMAKE_CURRENT_BINARY_DIR}"

REQUIRES
    "asic"
    "connect"
    "dns_server"
    "stratum"
    "stratum_v2"

PRIV_REQUIRES
    "app_update"
    "esp_adc"
    "esp_app_format"
    "esp_driver_gpio"
    "esp_event"
    "esp_http_server"
    "esp_lcd"
    "esp_netif"
    "esp_psram"
    "esp_timer"
    "esp_wifi"
    "nvs_flash"
    "spiffs"
    "vfs"
    "esp_driver_i2c"
    "esp_driver_uart"
    "tcp_transport"
    "esp_mm"
    "bt"

EMBED_FILES "http_server/recovery_page.html"
)

idf_build_set_property(COMPILE_OPTIONS "-DLV_CONF_INCLUDE_SIMPLE=1" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DLV_CONF_PATH=\"${CMAKE_SOURCE_DIR}/main/lv_conf.h\"" APPEND)

# main/log_buffer.c legt s_buffer[LOG_BUFFER_SIZE] (512 KB) mit EXT_RAM_NOINIT_ATTR an.
# Laut ESP-IDF (esp_common/include/esp_attr.h) faellt EXT_RAM_NOINIT_ATTR auf
# __NOINIT_ATTR zurueck, sobald CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY
# nicht gesetzt ist. Der 512-KB-Puffer landet dann im internen DRAM, und weil der
# ESP32-S3 dram0_0_seg auf maximal 0x54700 (341 KB) begrenzt ist, bricht der Link
# mit "region `dram0_0_seg' overflowed by ~360000 bytes" ab.
#
# Diese Pruefung macht den stillen Rueckfall sichtbar, bevor jemand stundenlang
# einen Reboot-Loop an einem echten Bug sucht.
foreach(_esp_miner_seg_opt
        SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY
        SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY)
    set(_esp_miner_seg_value "")
    file(STRINGS "${CMAKE_SOURCE_DIR}/sdkconfig" _esp_miner_sdkconfig_lines)
    foreach(_esp_miner_line IN LISTS _esp_miner_sdkconfig_lines)
        if("${_esp_miner_line}" MATCHES "^CONFIG_${_esp_miner_seg_opt}=(.+)$")
            set(_esp_miner_seg_value "${CMAKE_MATCH_1}")
        elseif("${_esp_miner_line}" MATCHES "^# CONFIG_${_esp_miner_seg_opt} is not set$")
            set(_esp_miner_seg_value "n")
        endif()
    endforeach()
    if(NOT _esp_miner_seg_value STREQUAL "y")
        message(FATAL_ERROR
            "sdkconfig-Fehler: CONFIG_${_esp_miner_seg_opt} ist nicht 'y'.\n"
            "main/log_buffer.c braucht externen RAM fuer seinen Log-Ringpuffer "
            "(EXT_RAM_NOINIT_ATTR). Ohne diese Option landet er im internen DRAM "
            "und fraesst die Reserve, die sonst fuer Task-Stacks frei bleibt.\n"
            "Fix: CONFIG_${_esp_miner_seg_opt}=y setzen oder sdkconfig aus "
            "sdkconfig.defaults neu erzeugen (idf.py fullclean && rm sdkconfig).")
    endif()
endforeach()

set(WEB_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/http_server/axe-os")

if("$ENV{GITHUB_ACTIONS}" STREQUAL "true")
    message(STATUS "Running on GitHub Actions. Web ui will be prebuilt.")

    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/embedded_web_ui.c" "${CMAKE_CURRENT_BINARY_DIR}/embedded_web_ui.h"
        COMMAND ${PYTHON} "${CMAKE_SOURCE_DIR}/tools/embed_web_ui.py"
                "${WEB_SRC_DIR}/dist/axe-os"
                "${CMAKE_CURRENT_BINARY_DIR}/embedded_web_ui.c"
                "${CMAKE_CURRENT_BINARY_DIR}/embedded_web_ui.h"
        DEPENDS "${CMAKE_SOURCE_DIR}/tools/embed_web_ui.py"
        COMMENT "Generating embedded Web UI source files (CI)"
        VERBATIM
    )

    # Make the binary compile depend on the generated source file in CI
    add_custom_target(web_ui_gen DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/embedded_web_ui.c")
    add_dependencies(${COMPONENT_LIB} web_ui_gen)


else()
    find_program(NPM_EXECUTABLE npm)
    if(NOT NPM_EXECUTABLE AND NOT EXISTS ${WEB_SRC_DIR}/dist)
        message(FATAL_ERROR "npm is not found! Please install it to proceed.")
    endif()

    # OpenAPI generation - only when openapi.yaml changes
    set(OPENAPI_SPEC "${CMAKE_CURRENT_SOURCE_DIR}/http_server/openapi.yaml")
    set(OPENAPI_STAMP "${CMAKE_BINARY_DIR}/openapi_generate.stamp")

    add_custom_command(
        OUTPUT ${OPENAPI_STAMP}
        COMMAND ${NPM_EXECUTABLE} i
        COMMAND ${NPM_EXECUTABLE} run generate:api
        COMMAND ${CMAKE_COMMAND} -E touch ${OPENAPI_STAMP}
        WORKING_DIRECTORY ${WEB_SRC_DIR}
        DEPENDS ${OPENAPI_SPEC}
        COMMENT "Generating API client from OpenAPI spec (openapi.yaml changed)"
        VERBATIM
        USES_TERMINAL
    )

    # Collect all frontend source files to track dependencies (excluding auto-generated code)
    file(GLOB_RECURSE WEB_UI_SOURCES
        "${WEB_SRC_DIR}/src/*.*"
    )
    list(FILTER WEB_UI_SOURCES EXCLUDE REGEX "/generated/")

    # Also track build configuration files
    list(APPEND WEB_UI_SOURCES
        "${WEB_SRC_DIR}/package.json"
        "${WEB_SRC_DIR}/package-lock.json"
        "${WEB_SRC_DIR}/angular.json"
        "${WEB_SRC_DIR}/tsconfig.json"
        "${WEB_SRC_DIR}/tsconfig.app.json"
    )

    # Use a stamp file to track when the build was last done
    set(WEB_UI_STAMP "${CMAKE_BINARY_DIR}/web_ui_build.stamp")
    # Track index.html.gz which is generated as part of the build
    set(WEB_UI_OUTPUT "${WEB_SRC_DIR}/dist/axe-os/index.html.gz")

    # Custom command that only runs when source files change
    add_custom_command(
        OUTPUT ${WEB_UI_STAMP} ${WEB_UI_OUTPUT}
        COMMAND ${NPM_EXECUTABLE} i
        COMMAND ${NPM_EXECUTABLE} run build:no-api-gen
        COMMAND ${CMAKE_COMMAND} -E touch ${WEB_UI_STAMP}
        WORKING_DIRECTORY ${WEB_SRC_DIR}
        DEPENDS ${WEB_UI_SOURCES} ${OPENAPI_STAMP}
        COMMENT "Building Web UI (source files changed)"
        VERBATIM
        USES_TERMINAL
    )

    # Create a target for the web UI build
    add_custom_target(web_ui_dist ALL
        DEPENDS ${WEB_UI_STAMP} ${OPENAPI_STAMP}
    )

    add_dependencies(${COMPONENT_LIB} web_ui_dist)

    # Generate embedded C files from the build output directory
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/embedded_web_ui.c" "${CMAKE_CURRENT_BINARY_DIR}/embedded_web_ui.h"
        COMMAND ${PYTHON} "${CMAKE_SOURCE_DIR}/tools/embed_web_ui.py"
                "${WEB_SRC_DIR}/dist/axe-os"
                "${CMAKE_CURRENT_BINARY_DIR}/embedded_web_ui.c"
                "${CMAKE_CURRENT_BINARY_DIR}/embedded_web_ui.h"
        DEPENDS ${WEB_UI_STAMP} "${CMAKE_SOURCE_DIR}/tools/embed_web_ui.py"
        COMMENT "Generating embedded Web UI source files"
        VERBATIM
    )

    # In local build, make the component depend on the generated file too
    add_custom_target(web_ui_gen DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/embedded_web_ui.c")
    add_dependencies(${COMPONENT_LIB} web_ui_gen)


endif()

# Per-device build settings (see nvs_config.h). Empty in the general release.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ESP_MINER_FALLBACK_ENABLED=${ESP_MINER_FALLBACK_ENABLED}
    ESP_MINER_FALLBACK_IP="${ESP_MINER_FALLBACK_IP}"
    ESP_MINER_FALLBACK_GW="${ESP_MINER_FALLBACK_GW}"
    ESP_MINER_FALLBACK_MASK="${ESP_MINER_FALLBACK_MASK}"
ESP_MINER_FALLBACK_DNS="${ESP_MINER_FALLBACK_DNS}"
    ESP_MINER_WIFI_BSSID="${ESP_MINER_WIFI_BSSID}"
    ESP_MINER_RELEASE_TAG="${ESP_MINER_RELEASE_TAG}"
)
