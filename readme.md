<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/logo-dark.svg">
    <img src="docs/assets/logo.svg" alt="ESP-Miner" width="520">
  </picture>
</p>

<h1 align="center">ESP-Miner</h1>

<p align="center">
  <strong>Hardened firmware for Bitaxe Bitcoin ASIC miners</strong><br>
  ESP32-S3 · BM1366 / BM1370 · Stratum V1 and V2 · AxeOS web interface
</p>

<p align="center">
  <a href="https://github.com/timfromhcs/ESP-Miner/actions/workflows/build.yml"><img alt="Build Firmware" src="https://github.com/timfromhcs/ESP-Miner/actions/workflows/build.yml/badge.svg"></a>
  <a href="https://github.com/timfromhcs/ESP-Miner/actions/workflows/unittest.yml"><img alt="Unit Test" src="https://github.com/timfromhcs/ESP-Miner/actions/workflows/unittest.yml/badge.svg"></a>
  <a href="https://github.com/timfromhcs/ESP-Miner/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/timfromhcs/ESP-Miner?sort=semver"></a>
  <a href="LICENSE"><img alt="License: GPL-3.0" src="https://img.shields.io/badge/license-GPL--3.0-blue.svg"></a>
</p>

This is a hardening fork of [bitaxeorg/ESP-Miner](https://github.com/bitaxeorg/ESP-Miner). It
focuses on what happens when a miner loses its router, its pool or its ASIC, and on making every
claim traceable to a test or a measurement. Each claim in this README carries a label for how it
was established (see [Evidence](#evidence--truth-classification)). Earlier claims that turned out
to be wrong are corrected in place, not silently removed.

**Not affiliated with the Bitaxe project.** Board names and hardware details come from this
repository's source (`main/device_config.h`).

## Contents

- [Status](#status)
- [Quick start](#quick-start)
- [Build](#build)
- [Test](#test)
- [Security and known limitations](#security-and-known-limitations)
- [Firmware deep dive](docs/FIRMWARE_DEEP_DIVE.md) — what this fork changes, measurements, hardware, architecture
- [Evidence](#evidence--truth-classification)
- [Contributing](#contributing) · [License](#license)

---

## Status

| Release | State |
|---|---|
| [`v4.0.1-stable`](https://github.com/timfromhcs/ESP-Miner/releases/tag/v4.0.1-stable) | latest Stable |
| `v4.0.2-rc1` | release candidate, merged to `master`, **not published as a release**: Wi-Fi reconnect, OTA-safety and BSSID-pin fixes. See [release readiness](docs/RELEASE_READINESS.md) |

| Unit | Board | ASIC | Firmware | State |
|---|---|---|---|---|
| `bitaxe-tim2` — `192.168.178.80` | **601 (Gamma)** | **BM1370** | `v4.0.2-rc1-tim2` | flashed over Wi-Fi on 2026-10-29, back on the same address in ~10 s; observation: [report](reports/v4.0.2-rc1/HARDWARE_VALIDATION.md) |
| `timsminer` — `192.168.178.61` | 201 (Ultra) | BM1366 | `v4.0.2-rc1-timsminer` | flashed 2026-10-29, BSSID-pinned, mining; `v4.0.0` was `HARDWARE VALIDATED` (353 h, 0.299 % stale) — this firmware is not yet validated on the unit. Hashrate below its earlier ~430 GH/s, under investigation |
| `blackharkminer` — `192.168.178.66` | 201 (Ultra) | BM1366 | `v4.0.2-rc1-blackharkminer` | flashed 2026-10-29, BSSID-pinned, mining; `v4.0.0` was `HARDWARE VALIDATED` (353 h, 0.290 % stale) — this firmware is not yet validated on the unit |

The two Ultras mine to a single wallet on zpool (distinct worker names). Their
Wi-Fi is pinned to one mesh node; see *Build* for why that pin is required.

Both BM1366 units were moved to v4.0.2-rc1 on 2026-10-29. Their `v4.0.0` validation figures are kept
above for reference; they do not describe the current firmware. Changes to shared code are marked in the changelog.

> **Correction (v4.0.2-rc1):** earlier versions of this README and the changelog
> called board 601 a "Bitaxe Supra". In this firmware board 601 maps to
> `FAMILY_GAMMA` (BM1370, `main/device_config.h`); "Supra" is the BM1368 family.
> The unit reports `deviceModel: Gamma` itself. The measurements are unaffected.

---

## Quick start

### Which file to flash

| File | Use it for |
|---|---|
| `esp-miner.bin` | **Over-the-air updates.** Application image only. |
| `esp-miner-factory-<board>-<version>.bin` | **USB flashing** of a blank or bricked unit. Full flash image: bootloader, partition table, app, web UI, board config. |

Since `v4.0.2-rc1` the OTA endpoint refuses a factory/merged image, a foreign
application or an image for another chip before anything becomes bootable.

### Update over Wi-Fi

1. Check the unit: `GET http://<miner>/api/system/info` → `boardVersion`,
   `ASICModel`, `version`, and `apEnabled == 0` (OTA is refused in AP mode).
2. Note the image's SHA-256 (`Get-FileHash` / `sha256sum`).
3. Upload **once**:

   ```bash
   curl --fail-with-body -H 'Content-Type: application/octet-stream' --data-binary @esp-miner.bin http://<miner>/api/system/OTA
   ```

4. The miner answers `Firmware update complete, rebooting now!` and restarts.
   A dropped connection at this point is normal — **do not send it again**.
   Wait and poll the same address; `version` and `runningPartition` change.

The AxeOS *Update* page (`/update`) does the same from the browser.

**Per-device builds stay per-device.** A variant from `tools/build_release.sh`
can pin the station MAC, the BSSID and a static fallback address that are
reserved on one router for one unit. Flashing the general image onto such a unit
drops the pinned MAC and with it the DHCP reservation; flashing a variant onto
another unit duplicates MAC and IP. The firmware cannot detect either mistake.

**There is no automatic rollback.** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is
off. The previous image stays in the other OTA slot and can be selected with
`POST /api/system/boot` and `{"partition":"ota_0"}` (or `ota_1`) as long as the
new image serves HTTP; otherwise recover over USB. Details:
[OTA safety](docs/OTA_SAFETY.md).

### Recover over USB

```bash
python -m esptool --chip esp32s3 -p COM3 -b 460800 write_flash 0x0 esp-miner-factory-<board>-<version>.bin
```

From a build directory instead:

```bash
python -m esptool --chip esp32s3 -p COM3 -b 460800 write_flash 0x0 build-tim2/bootloader/bootloader.bin 0x8000 build-tim2/partition_table/partition-table.bin 0x10000 build-tim2/esp-miner.bin 0xf10000 build-tim2/ota_data_initial.bin
```

Partition layout: `factory` at `0x10000`, `www` (3 MiB) at `0x410000`, `ota_0`
at `0x710000` and `ota_1` at `0xB10000` (4 MiB each), `otadata` at `0xF10000`.
A unit flashed only from a factory image has empty OTA slots; the first OTA
fills `ota_0` and later ones alternate.

---

## Build

Requires ESP-IDF **v6.0.2** and Node.js **v22+** (CI uses 24.14.0). The web UI is
built and embedded automatically.

```bash
. ~/esp/esp-idf/export.sh

# General image: no baked-in address, no per-device identity -> build/esp-miner.bin
idf.py build

# Named per-device variants -> dist/<variant>/esp-miner-<variant>.bin
./tools/build_release.sh tim2                 # Gamma, board 601, BM1370

# Ultra variants: <ip> <gateway> <netmask> <dns> <bssid|none>
./tools/build_release.sh timsminer <ip> <gateway> <netmask> <dns> <bssid>
./tools/build_release.sh blackharkminer <ip> <gateway> <netmask> <dns> <bssid>
```

The BSSID pin is **mandatory** for `timsminer` / `blackharkminer` (pass `none`
only on a single-access-point network). On a mesh the station otherwise picks
the loudest node after a reboot, which can be one that associates but relays
nothing; an OTA-flashed unit then goes offline and needs a USB cable.

Per-device values are passed with `-D` on every build rather than trusted from
the CMake cache: a stale cache entry otherwise keeps the old value silently —
including `PROJECT_VER`, which is how a unit ends up reporting `v3` while the
source says `v4`.

## Test

```bash
# Host tests: pure policy/parser code, no IDF, no hardware
cd simulation && make test

# Same suites under AddressSanitizer + UndefinedBehaviorSanitizer
make clean && make CFLAGS="-Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I." && make run

# Firmware unit tests (Unity) in QEMU; needs qemu-system-xtensa and a checkout with symlinks (Linux/WSL)
./tools/run_qemu_tests.sh

# AxeOS frontend, headless Chrome
cd main/http_server/axe-os && npm ci && npm run test:ci
```

Host suites: notify validator, compact-target differential test against an
independent reference decoder, thermal/power policy, network recovery ladder and
reconnect schedule, OTA upload gate, and a simulation model of the board. What
each one proves — and what it does not: [test matrix](docs/TEST_MATRIX.md).

---

## Security and known limitations

- **The web API is plain HTTP without authentication**, limited only by a
  local-network check — including `POST /api/system/OTA`. Never expose a miner
  to an untrusted network or forward its port to the internet.
- **OTA images are not signed.** The upload is checked for being an ESP32-S3
  esp-miner application image and ESP-IDF verifies its checksum; neither is
  authentication.
- **No automatic rollback** (see Quick start).
- **Pool credentials appear in the device log.** The `mining.authorize` request
  is logged at INFO level and the log is readable through the API. Fix planned
  before the next Stable.
- **AxeOS dependencies** have open advisories (Angular 19.2.20), assessed as low
  exploitability for this app; upgrade tracked.
- **Version rolling is 16 bits wide.** A pool mask of `0xffffffff` does not mean
  "roll everything": bits above 28 are BIP9 signalling, and the firmware clamps
  to the reachable window.
- **Stratum V2 extended channels** have working framing and handshake; channel
  multiplexing is still in development.
- **Router-outage recovery** is covered by host policy tests, not yet measured on
  hardware with a real outage.

Report vulnerabilities through this repository's
[security advisories](https://github.com/timfromhcs/ESP-Miner/security/advisories/new);
see [SECURITY.md](SECURITY.md).

---

## What this fork changes

- **Wi-Fi and DHCP recovery:** reconnects no longer stop on handshake-type errors. The DHCP fallback replays only a complete earlier lease; a static fallback exists but stays off unless set. The station can be pinned to one mesh node (BSSID).
- **Safer OTA:** an upload is checked (image magic, chip, app descriptor, project, size) before the boot partition changes. A failed `esp_ota_begin` returns an error instead of aborting the device, and a stalled client times out.
- **BM1370 parity:** ticket mask follows the pool difficulty, UART baud matches the chip's divider, and version-rolling bits are rebuilt with the mask.
- **Tests:** host suites for the protocol parsers and recovery policy, a QEMU unit suite, sanitizer runs in CI.

The full write-up, with measurements and their limits: [docs/FIRMWARE_DEEP_DIVE.md](docs/FIRMWARE_DEEP_DIVE.md).

---

## Evidence & truth classification

| Claim | Evidence | Classification |
|---|---|---|
| Overt ASICBoost (BIP310) on BM1366 | Register `0xA4` write + accepted rolled shares | **PROVEN** |
| Overt ASICBoost on BM1370 | Register `0xA4` write at 65 535 rolls, rolled shares accepted | **PROVEN** |
| BM1370 ticket mask tracks pool difficulty | Live log: 256 → 4 096 → 1 000 across `mining.set_difficulty` | **PROVEN** |
| Host/chip UART rate agreement | Log: `BT8D=2 -> 1041666 baud (host programmed to match)` | **PROVEN** |
| Notify counters track notifies | API: `notifyReceived` > 0 and rising; was 0 for the whole prior session | **PROVEN** |
| Compact-target validation matches Core | 20 550 differential checks incl. 20 000 random words | **PROVEN** (host) |
| Network recovery ladder and reconnect schedule | 92 host checks | **PROVEN** (host) |
| Reconnect after a real router outage | - | **NOT MEASURED** |
| OTA gate rejects factory/foreign images | host tests incl. the real v4.0.1 factory image | **TESTED** (host); handler not yet exercised on hardware |
| Automatic rollback | disabled in `sdkconfig` | **not available** |
| ASIC-side midstate reuse | Unconfirmed in silicon | **NOT PROVEN** |

Retracted claims are kept in the repository with the retraction attached — see
`docs/PLAN_V2_17_STALE_AND_LIMITS.md`. A wrong diagnosis that is silently
replaced is a wrong diagnosis that gets re-applied.

---

## Contributing

- Development guide: [`AGENTS.md`](AGENTS.md)
- Engineering specification and release gates: [`GEMINI.md`](GEMINI.md)
- Changelog: [`CHANGELOG.md`](CHANGELOG.md)
- Firmware deep dive: [`docs/FIRMWARE_DEEP_DIVE.md`](docs/FIRMWARE_DEEP_DIVE.md)
- OTA safety: [`docs/OTA_SAFETY.md`](docs/OTA_SAFETY.md) · Network recovery: [`docs/NETWORK_RECOVERY_DESIGN.md`](docs/NETWORK_RECOVERY_DESIGN.md)
- Test matrix: [`docs/TEST_MATRIX.md`](docs/TEST_MATRIX.md) · Release readiness: [`docs/RELEASE_READINESS.md`](docs/RELEASE_READINESS.md)
- Evidence index: [`docs/EVIDENCE.md`](docs/EVIDENCE.md)

Bug reports and patches are welcome as pull requests against `master`. Please keep each claim
backed by a test or a measurement, and label it as in the [Evidence](#evidence--truth-classification) table.

## License

GPL-3.0 — see [LICENSE](LICENSE).
