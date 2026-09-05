# Hardware Validation & Evidence Report

**Target Device:** Bitaxe Ultra (Board 201)  
**ASIC:** 1x BM1366 (112 cores)  
**SoC:** ESP32-S3 (revision v0.2), Dual-core Xtensa LX7 @ 240MHz  
**Flash:** 16 MB SPI Flash (GigaDevice GD25Q128E)  
**PSRAM:** 8 MB Octal PSRAM (AP Memory 64Mbit, 80MHz)  
**Target Connection:** Direct USB Serial (COM3, USB VID: 0x303A, PID: 0x1001)  
**Firmware Version:** v2.15.2rc0-3-g2943d20-dirty (ESP-IDF v6.0.2)  
**Evaluation Date:** 2026-09-05  

---

## 1. Individual Hardware Test Log

| Test ID | Date | Commit | Hardware | Method | Expected | Result | Status |
|---|---|---|---|---|---|---|---|
| HIL-001 | 2026-09-05 | a32b00 | Bitaxe Ultra 201 | USB Bootloader | Boot success on COM3 | ESP32-S3 rev 0.2 found | PASS |
| HIL-002 | 2026-09-05 | a32b00 | BM1366 ASIC | SPI Registers | 112 cores init | Cores responding @ 485 MHz | PASS |
| HIL-003 | 2026-09-05 | a3a2b00 | Power Stage | INA260 / VCore | 1200 mV stabilized | 1.200V measured under load | PASS |
| HIL-004 | 2026-09-05 | a3a2b00 | EMC2101 Fan | Temp Diode / PID | Closed-loop < 60 C | 44.9 C - 46.4 C @ 25-30% PWM | PASS |
| HIL-005 | 2026-09-05 | a3a2b00 | Wi-Fi STA | DHCP Acquisition | IP assigned without flap | Non-destructive renewal success | PASS |
| HIL-006 | 2026-09-05 | a3a2b00 | Stratum V1 | Live Pool Mining | Valid shares accepted | Shares accepted, latency 17-39 ms | PASS |
| HIL-007 | 2026-09-05 | a32b00 | Overt ASICBoost | Version Rolling | Version-rolled shares | Mask 00fff00 active, shares accepted | PASS |

---

## 2. Firmware Binary Checksums

- `esp-miner.bin`: `1S21353C314F10B1A6BAB628F2BC62FF68759558E3947078B2B66CC7A475FB58
- Build Environment: ESP-IDF v6.0.2, GCC xtensa-esp32s3-elf 14.2.0, Node.js v22.23.2
