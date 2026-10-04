#!/usr/bin/env bash
# Build one release variant of the ESP-Miner firmware.
#
#   ./tools/build_release.sh general
#   ./tools/build_release.sh timsminer 192.168.178.66
#   ./tools/build_release.sh blackharkminer 192.168.178.61
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
    timsminer|blackharkminer)
        if [ $# -lt 2 ]; then
            echo "usage: $0 ${VARIANT} <static-ip> [gateway] [netmask] [dns]" >&2
            exit 2
        fi
        IP="$2"
        GW="${3:-192.168.178.1}"
        MASK="${4:-255.255.255.0}"
        # Resolver for the baked fallback.
        #
        # Defaulting this to the gateway is what makes the failure look like a dead
        # network: name resolution then depends on the same mesh hop the address
        # depends on, and one broken relay takes out both. Pointing at public
        # resolvers removes that dependency, so a mesh that cannot relay still
        # resolves names and can reach the pool.
        DNS="${5:-1.1.1.1 9.9.9.9}"
        # BSSID of the access point to pin to, e.g. 2c:3a:fd:49:d3:95.
        BSSID="${6:-}"
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