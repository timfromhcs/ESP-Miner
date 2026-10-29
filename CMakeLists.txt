# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# (Not part of the boilerplate)
# This example uses an extra component for common functions such as Wi-Fi and Ethernet connection.
# set(EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/common_components/protocol_examples_common)

set(ESP_MINER_BASE_VERSION "v4.0.2-rc1")

# PROJECT_VER is a CACHE entry, so an existing build directory keeps whatever
# version it was first configured with. Without FORCE the source-side bump is
# silently ignored and the device goes on reporting the old version - which looks
# exactly like "the OTA did not take" and sends you looking at the bootloader
# instead of at the build directory. This bit a device that reported v3 while the
# source said v4.
set(PROJECT_VER "${ESP_MINER_BASE_VERSION}" CACHE STRING "Firmware version reported by the API and /version" FORCE)

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

# Station MAC to present, "aa:bb:cc:dd:ee:ff" or "" for the eFuse MAC. Empty in
# the general release. A router that refuses to serve a MAC cannot be argued with
# from the firmware, but a unit whose identity has to be pinned (a reservation, a
# bridge rule, an ACL) needs the address to be identical on every boot and on
# every build, and efuse MACs are not.
set(ESP_MINER_STA_MAC           "" CACHE STRING "Station MAC override")

# Hostname sent in DHCP option 12. "" keeps the hostname from NVS. Set it only
# when the server rejects the DISCOVER; some DHCP servers drop a request whose
# option 12 is empty or malformed, which looks exactly like "the network is down".
set(ESP_MINER_DHCP_HOSTNAME     "" CACHE STRING "Hostname for DHCP option 12")

# Bring up the configuration access point when the unit is stuck without an IP.
# 0 = never, 1 = always (general builds leave it off, the web UI is reachable over
# the LAN instead), 2 = only when no address could be bound at all.
set(ESP_MINER_CONFIG_AP_STUCK  2 CACHE STRING "Enable the config AP when stuck without an IP")

# Per-device hardware identity, used as the default for the boardversion /
# devicemodel / asicmodel NVS entries (see nvs_config.h). The universal
# "I don't know" values in the general build; a per-device release passes the
# real ones so a unit that lost its NVS still knows what it is:
#   idf.py -DESP_MINER_BOARD_VERSION=601 -DESP_MINER_DEVICE_MODEL=gamma \
#          -DESP_MINER_ASIC_MODEL=BM1370 build
set(ESP_MINER_BOARD_VERSION "000"     CACHE STRING "Fallback board version when NVS has none")
set(ESP_MINER_DEVICE_MODEL  "unknown" CACHE STRING "Fallback device model when the board version is not a known one")
set(ESP_MINER_ASIC_MODEL    "unknown" CACHE STRING "Fallback ASIC model when the board version is not a known one")

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

# These three have non-empty general defaults, so the rule above would never
# overwrite a stale cache entry for them. A per-device build has to win.
foreach(_v ESP_MINER_BOARD_VERSION ESP_MINER_DEVICE_MODEL ESP_MINER_ASIC_MODEL)
    if(DEFINED ${_v})
        set(${_v} "${${_v}}" CACHE STRING "" FORCE)
    endif()
endforeach()

# Same problem as above: these have a non-empty general default ("" or 2), so a
# per-device build must be able to overwrite a stale cache entry.
foreach(_v ESP_MINER_STA_MAC ESP_MINER_DHCP_HOSTNAME ESP_MINER_CONFIG_AP_STUCK)
    if(DEFINED ${_v})
        set(${_v} "${${_v}}" CACHE STRING "" FORCE)
    endif()
endforeach()

message(STATUS "ESP-Miner release tag    : ${ESP_MINER_RELEASE_TAG}")
message(STATUS "ESP-Miner fallback IP    : ${ESP_MINER_FALLBACK_IP}")
message(STATUS "ESP-Miner fallback DNS   : ${ESP_MINER_FALLBACK_DNS}")
message(STATUS "ESP-Miner wifi BSSID pin : ${ESP_MINER_WIFI_BSSID}")
message(STATUS "ESP-Miner version        : ${PROJECT_VER}")
message(STATUS "ESP-Miner hardware       : board=${ESP_MINER_BOARD_VERSION} model=${ESP_MINER_DEVICE_MODEL} asic=${ESP_MINER_ASIC_MODEL}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)


project(esp-miner)

