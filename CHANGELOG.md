# Changelog

All notable changes to this project are documented in this file.
This project adheres to [Semantic Versioning](https://semver.org/) and the **Truth Contract**.

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
