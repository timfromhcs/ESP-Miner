# Hardware Validation & Evidence Report

## Target Devices

### Device A (Primary Hardware Validation)
- **Target IP:** `192.168.178.66`
- **MAC Address:** `74:4D:BD:77:DD:3C`
- **Board:** Bitaxe Ultra (Board 201)
- **ASIC:** 1× BM1366 (112 core clusters, 894 small engines)
- **SoC:** ESP32-S3 (revision v0.2), Dual-core Xtensa LX7 @ 240MHz
- **Flash:** 16 MB SPI Flash (GigaDevice GD25Q128E)
- **PSRAM:** 8 MB Octal PSRAM (AP Memory 64Mbit, 80MHz)
- **Primary Connection:** USB Serial (`COM3`, VID: `0x303A`, PID: `0x1001`) & Wi-Fi
- **Firmware Version:** `v2.15.3-hardened` (ESP-IDF v6.0.2)

### Device B (Secondary Independent Production Device)
- **Target IP:** `192.168.178.61`
- **MAC Address:** `74:4D:BD:77:99:80`
- **Hostname:** `blackharkminer`
- **Board:** Bitaxe Ultra (Board 201)
- **ASIC:** 1× BM1366 (112 core clusters, 894 small engines)
- **Previous Firmware:** `v2.14.0` (Uptime: 1,618,749 s)
- **Upgraded Firmware:** `v2.15.3-hardened` (ESP-IDF v6.0.2)
- **Primary Connection:** Network Wi-Fi OTA (`http://192.168.178.61/api/system/OTA`)
- **Validation Scope:** Backup -> Safe OTA -> Restore -> Sustained Mining -> Recovery

---

## 1. Hardware-in-the-Loop (HIL) Test Log

| Test ID | Device | Date | Commit | Hardware | Method | Result | Status |
|---|---|---|---|---|---|---|---|
| HIL-001 | Device A | 2026-09-05 | a3a2b00 | Bitaxe Ultra 201 | USB Bootloader | ESP32-S3 rev 0.2 enumerated on COM3 | PASS |
| HIL-002 | Device A | 2026-09-05 | a3a2b00 | BM1366 ASIC | SPI Registers | 112 cores responding @ 485 MHz | PASS |
| HIL-003 | Device A | 2026-09-05 | a3a2b00 | Power Stage | INA260 / VCore | 1.206 V measured under load (12.40 W) | PASS |
| HIL-004 | Device A | 2026-09-05 | a3a2b00 | EMC2101 Fan | Temp Diode / PID | Closed-loop 58.1 °C @ 2,400 RPM | PASS |
| HIL-005 | Device A | 2026-09-05 | a3a2b00 | Wi-Fi STA | DHCP Acquisition | IP acquired in 1,654 ms; reconnect in 2,630 ms | PASS |
| HIL-006 | Device A | 2026-09-05 | a3a2b00 | Stratum V1 | Live Pool Mining | 67 shares submitted, 0 rejects (0.00%) | PASS |
| HIL-007 | Device A | 2026-09-05 | a3a2b00 | Overt ASICBoost | Version Rolling | Mask 001fff00 active, 0 duplicate shares | PASS |
| HIL-008 | Device A | 2026-09-06 | 655787b | Wi-Fi Fast Scan | Fast Channel Scan | Connected without full band scan stalls | PASS |
| HIL-009 | Device A | 2026-09-06 | 655787b | Radio Coexist | BLE Grace Window | BLE deferred 15s during STA connect | PASS |
| HIL-010 | Device A | 2026-09-06 | 655787b | Mining Runtime | 600s Benchmark | 435.81 GH/s mean, 28.45 J/TH, 0 rejects | PASS |
| HIL-011 | Device B | 2026-09-06 | e399cf5 | NVS / Settings | REST API Extraction | Complete private backup outside repo | PASS |
| HIL-012 | Device B | 2026-09-06 | e399cf5 | OTA Subsystem | Binary POST OTA | Safe upload to ota_0, reboot in 2.0s | PASS |
| HIL-013 | Device B | 2026-09-06 | e399cf5 | NVS Restoration | Config Verification | 10/10 parameters verified restored | PASS |
| HIL-014 | Device B | 2026-09-06 | e399cf5 | Sustained Mining| 300s Telemetry Run | 434.22 GH/s mean, 28.35 J/TH, 0 rejects | PASS |
| HIL-015 | Device B | 2026-09-06 | e399cf5 | State Recovery | Pause / Resume API | Non-destructive recovery, 0 reboots | PASS |

---

## 2. Firmware Binary Checksums

- `build_wsl/esp-miner.bin`: `28d37dfabc33e5732c5f383f169d039dc02867b2c9a422d63f7ac331cec3965c`
- Release Version: `v2.15.3-hardened`
- Toolchain: ESP-IDF v6.0.2, GCC xtensa-esp32s3-elf 14.2.0, Angular 19+ Axe-OS
