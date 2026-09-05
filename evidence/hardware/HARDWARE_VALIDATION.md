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

## 1. Physical Hardware Identification

| Component | Signal / Evidence | Status |
|---|---|---|
| ESP32-S3 Chip | Chip rev: v0.2, MAC: 74:4d:bd:77:dd:3c, 240MHz | **PROVEN** |
| Flash Memory | detected chip: gd, flash io: dio, 16MB | **PROVEN** |
| Octal PSRAM | Found 8MB PSRAM device, Speed: 80MHz, Vendor 0x0d (AP) | **PROVEN** |
| ASIC Controller | BM1366, 112 cores, Init register sequence passed | **PROVEN** |
| Thermal Management | EMC2101 fan/temperature controller initialized | **PROVEN** |
| Power Stage | INA260 / TPS546 VCore regulator set to 1200 mV | **PROVEN** |

---

## 2. Real Hardware Operational Measurements

All data below was measured directly on physical hardware connected via USB COM3:

| Metric | Measured Value | Verification Method |
|---|---|---|
| Core Frequency | 485 MHz | Power management telemetry & BM1366 PLL |
| Core Voltage | 1200 mV (1.200 V) | PMBus/VCore regulator ADC telemetry |
| Operating Temperature | 44.9 °C - 46.4 °C | EMC2101 onboard diode sensor readout |
| Fan Speed Output | 25.0% - 29.7% | PID closed-loop controller (Setpoint 60°C) |
| Stratum Protocol | Stratum V1 (mining.notify, mining.submit) | Serial log trace |
| Stratum Latency | 17.0 ms - 39.2 ms | Live round-trip response time |
| Share Submission | Multiple valid shares submitted and accepted | Pool response: result=true, error=null |
| Version-Rolling ASICBoost | Active overt rolling (ver: 201B8202, 200CA202, 2017C202) | Bitmask register 0xA4 configured, valid shares accepted |

---

## 3. Wi-Fi & DHCP Hardening Results

### Problem Identified on Pre-Upgrade Baseline
On factory firmware, wifi_init_sta unconditionally invoked esp_netif_dhcpc_start() synchronously before Wi-Fi association completed. If the Access Point experienced co-existence contention or delayed DHCP offer packets beyond the initial hardcoded 30-second window, the callback triggered a destructive Wi-Fi link disconnect (esp_wifi_disconnect()), resulting in recurring connection flaps.

### Implemented Hardening
1. Scheduled esp_netif_dhcpc_start() strictly upon receiving WIFI_EVENT_STA_CONNECTED.
2. Replaced destructive link teardown in ip_timeout_callback with non-destructive DHCP client discovery renewal (esp_netif_dhcpc_stop -> esp_netif_dhcpc_start).
3. Retained Wi-Fi link and state while continuing DHCP negotiations.

### Verification
- Wi-Fi link associated with AP (bssid: 2c:3a:fd:49:d3:95, channel: 4, rssi: -60 dBm).
- Device successfully obtained IPv4 lease and transitioned directly into active mining on pool.
