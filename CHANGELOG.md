# Changelog

All notable changes to this project are documented in this file.
This project adheres to [Semantic Versioning](https://semver.org/) and the **Truth Contract**.

---

## [2.15.3-hardened] - 2026-09-06

### Changed
- **Wi-Fi Fast Scan:** Enabled `WIFI_FAST_SCAN` for known STA connections, significantly reducing scan overhead and channel dwell time during startup.
- **Selective SoftAP Initialization:** SoftAP is initialized strictly when no SSID is configured in NVS, preventing dual STA+AP coexistence radio contention on boot.
- **Exponential/Staged Wi-Fi Reconnect Backoff:** Replaced fixed 5s disconnect wait with staged 500ms -> 2000ms -> 5000ms retry intervals for rapid recovery from brief router drops.
- **BLE / Wi-Fi Coexistence Hardening:** Increased BLE advertising grace period on boot to 15s when an SSID exists, preventing NimBLE 2.4 GHz radio packet collision during critical DHCP handshake (e.g. on FRITZ!Box routers).
- **Stratum Reconnect Loop:** Reduced Wi-Fi link check delay in Stratum V1/V2 worker tasks from 10s to 1s, achieving near-instantaneous reconnection when the network recovers.

### Hardware Validation
- **Device A (192.168.178.66 / USB COM3):**
  - IP acquisition on FRITZ!Box: Instantaneous without radio contention (1,654 ms).
  - Mining: Sustained 435.81 GH/s mean @ 485 MHz / 1.206 V VCore (12.40 W, 28.45 J/TH).
  - Stratum: 66 shares accepted, 0 rejects (0.00%), 0 duplicate nonces.
- **Device B (192.168.178.61 / Network OTA):**
  - Upgraded via safe OTA from v2.14.0 to v2.15.3-hardened.
  - Complete pre-upgrade NVS backup and 100% parameter restoration verified.
  - Mining: Sustained 434.22 GH/s mean @ 485 MHz / 1.200 V (12.31 W, 28.35 J/TH).
  - Stratum: 37 shares accepted, 0 rejects (0.00%), completely eliminated 253 "Invalid job id" rejections from v2.14.0.
  - Recovery: Pause / Resume verified without reboot or stale work.

---

## [2.15.2-hardened] - 2026-09-05


### Added
- Architectural and Inspection documentation (`docs/ARCHITECTURE.md`, `docs/BUILD.md`, `docs/FLASHING.md`).
- Hardware-in-the-Loop validation evidence and test matrix (`docs/HARDWARE_VALIDATION.md`, `docs/EVIDENCE.md`).
- Release provenance and checksum manifest (`docs/RELEASE.md`, `evidence/hashes/manifest.json`).

### Changed
- Refactored Wi-Fi STA DHCP client startup sequence to trigger exclusively upon `WIFI_EVENT_STA_CONNECTED`.
- Replaced destructive `esp_wifi_disconnect()` calls on DHCP lease timeouts with non-destructive ``esp_netif_dhcpc_stop() ` + `esp_netif_dhcpc_start()` discovery renewals.

### Fixed
- Unnecessary Wi-Fi link flapping in heavily congested 2.4 GHz environments.

### Hardware Validation
- Validated via physical USB Serial (COM3) on Bitaxe Ultra (Board 201, BM1366 ASIC, 112 Cores).
- Measured: 485 MHz @ 1200 mV, 44.9 C - 46.4 C, live Stratum shares accepted by solopool.eu, Overt ASICBoost version-rolling register 0xA4 programming verified.

### Known Limitations
- ASIC-side midstate reuse remains NOT PROVEN due to OPAQUE silicon internals.
- BM1366 nonce space traversal cannot be externally partitioned beyond HCN calculation.
