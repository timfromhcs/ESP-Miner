#!/usr/bin/env bash
# Build one release variant of the ESP-Miner firmware.
#
#   ./tools/build_release.sh general
#   ./tools/build_release.sh timsminer <ip> <gw> <mask> <dns> <bssid|none>
#   ./tools/build_release.sh blackharkminer <ip> <gw> <mask> <dns> <bssid|none>
#   ./tools/build_release.sh tim2
#
# The general variant carries no baked-in address. A per-device variant is only
# correct when that address is reserved in the router for the unit's MAC - see
# main/nvs_config.h.
set -euo pipefail

VARIANT="${1:-general}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build-${VARIANT}"
TASK="${NPM_EXECUTABLE:-/usr/bin/npm}"

case "${VARIANT}" in
    general)
        DEFS=()
        ;;
    # Bitaxe Supra, board 601, BM1370.
    #
    # This one is not a network variant in the usual sense: it additionally pins
    # the hardware identity and the station MAC, because the unit lost its NVS at
    # least once and a unit that cannot describe itself comes up as an unknown
    # board with no valid family. ESP_MINER_STA_MAC is the deeper of the two: an
    # efuse MAC is not reproducible across builds, and this address is reserved in
    # the router for exactly this MAC.
    #
    # The values must stay byte-for-byte identical to what the unit already runs,
    # or the OTA flash will move the address out from under its DHCP reservation.
    tim2)
        DEFS=(
            -DESP_MINER_RELEASE_TAG="${VARIANT}"
            -DESP_MINER_FALLBACK_ENABLED=1
            -DESP_MINER_FALLBACK_IP="192.168.178.80"
            -DESP_MINER_FALLBACK_GW="192.168.178.1"
            -DESP_MINER_FALLBACK_MASK="255.255.255.0"
            -DESP_MINER_FALLBACK_DNS="192.168.178.1"
            -DESP_MINER_WIFI_BSSID="48:5d:35:0f:39:fc"
            -DESP_MINER_STA_MAC="28:84:85:a6:1b:e0"
            -DESP_MINER_DHCP_HOSTNAME=""
            -DESP_MINER_CONFIG_AP_STUCK=2
            -DESP_MINER_BOARD_VERSION="601"
            -DESP_MINER_DEVICE_MODEL="gamma"
            -DESP_MINER_ASIC_MODEL="BM1370"
        )
        ;;
    timsminer|blackharkminer)
        # The BSSID pin is mandatory, not optional. On a mesh every node broadcasts
        # the same SSID, and after a reboot the station takes the loudest one - which
        # can be a node that associates but relays nothing (no DHCP, gateway
        # unreachable). A unit flashed over OTA without a pin then goes offline and
        # needs a USB cable; this happened to blackharkminer on 2026-10-29. Pass
        # "none" to build without a pin deliberately (single access point).
        if [ $# -lt 6 ]; then
            echo "usage: $0 ${VARIANT} <static-ip> <gateway> <netmask> <dns> <bssid|none>" >&2
            echo "  bssid: the access point to pin to, e.g. aa:bb:cc:dd:ee:ff, or 'none' on a single-AP network" >&2
            exit 2
        fi
        IP="$2"
        GW="$3"
        MASK="$4"
        # Resolver for the baked fallback.
        #
        # Defaulting this to the gateway is what makes the failure look like a dead
        # network: name resolution then depends on the same mesh hop the address
        # depends on, and one broken relay takes out both. Pointing at public
        # resolvers removes that dependency, so a mesh that cannot relay still
        # resolves names and can reach the pool.
        DNS="$5"
        # BSSID of the access point to pin to, aa:bb:cc:dd:ee:ff, or "none".
        BSSID="$6"
        if [ "${BSSID}" = "none" ]; then
            BSSID=""
        elif ! [[ "${BSSID}" =~ ^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$ ]]; then
            echo "error: bssid '${BSSID}' is not aa:bb:cc:dd:ee:ff (or 'none')" >&2
            exit 2
        fi
        DEFS=(
            -DESP_MINER_RELEASE_TAG="${VARIANT}"
            -DESP_MINER_FALLBACK_ENABLED=1
            -DESP_MINER_FALLBACK_IP="${IP}"
            -DESP_MINER_FALLBACK_GW="${GW}"
            -DESP_MINER_FALLBACK_MASK="${MASK}"
            -DESP_MINER_FALLBACK_DNS="${DNS}"
            -DESP_MINER_WIFI_BSSID="${BSSID}"
        )
        ;;
    *)
        echo "unknown variant: ${VARIANT}" >&2
        exit 2
        ;;
esac

echo "=== building ${VARIANT} -> ${BUILD_DIR} ==="
. "${IDF_PATH}/export.sh" >/dev/null 2>&1

idf.py -B "${BUILD_DIR}" -DNPM_EXECUTABLE="${TASK}" "${DEFS[@]}" build

OUT="${ROOT}/dist/${VARIANT}"
mkdir -p "${OUT}"
cp "${BUILD_DIR}/esp-miner.bin" "${OUT}/esp-miner-${VARIANT}.bin"
echo "=== ${OUT}/esp-miner-${VARIANT}.bin ==="
ls -l "${OUT}/esp-miner-${VARIANT}.bin"