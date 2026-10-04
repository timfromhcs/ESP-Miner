# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# (Not part of the boilerplate)
# This example uses an extra component for common functions such as Wi-Fi and Ethernet connection.
# set(EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/common_components/protocol_examples_common)

set(PROJECT_VER "v4.0.0-stable" CACHE STRING "Firmware version reported by the API and /version")

# Per-device release tagging. The general build leaves this empty; a unit that is
# flashed with its own release gets a distinct version so the image can be told
# apart from the general one on the device and in AxeOS:
#   idf.py -DESP_MINER_RELEASE_TAG=timsminer build
set(ESP_MINER_RELEASE_TAG "" CACHE STRING "Per-device release suffix")
if(NOT ESP_MINER_RELEASE_TAG STREQUAL "")
    set(PROJECT_VER "${PROJECT_VER}-${ESP_MINER_RELEASE_TAG}")
endif()

# Per-device static fallback (see nvs_config.h). Empty in the general build.
set(ESP_MINER_FALLBACK_ENABLED 0 CACHE STRING "Enable the baked static IPv4 fallback")
set(ESP_MINER_FALLBACK_IP      "" CACHE STRING "Static IPv4 address reserved in the router")
set(ESP_MINER_FALLBACK_GW      "" CACHE STRING "Static gateway")
set(ESP_MINER_FALLBACK_MASK    "255.255.255.0" CACHE STRING "Static netmask")
set(ESP_MINER_FALLBACK_DNS     "" CACHE STRING "Static DNS resolver")
set(ESP_MINER_WIFI_BSSID       "" CACHE STRING "BSSID of the AP to pin the station to")

# Each variant builds into its own directory, but make the per-device values
# explicit rather than cache-dependent: a stale cache entry from a previous run of
# the same build directory would otherwise silently keep the old value, which
# produces a release that claims to be pinned to one node and is not.
foreach(_v
        ESP_MINER_RELEASE_TAG
        ESP_MINER_FALLBACK_ENABLED
        ESP_MINER_FALLBACK_IP
        ESP_MINER_FALLBACK_GW
        ESP_MINER_FALLBACK_MASK
        ESP_MINER_FALLBACK_DNS
        ESP_MINER_WIFI_BSSID)
    if(DEFINED ${_v} AND NOT "${${_v}}" STREQUAL "")
        set(${_v} "${${_v}}" CACHE STRING "" FORCE)
    endif()
endforeach()

message(STATUS "ESP-Miner release tag    : ${ESP_MINER_RELEASE_TAG}")
message(STATUS "ESP-Miner fallback IP    : ${ESP_MINER_FALLBACK_IP}")
message(STATUS "ESP-Miner fallback DNS   : ${ESP_MINER_FALLBACK_DNS}")
message(STATUS "ESP-Miner wifi BSSID pin : ${ESP_MINER_WIFI_BSSID}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)


project(esp-miner)

