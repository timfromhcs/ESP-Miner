# Bitaxe Ultra / BM1366 Hardware & Firmware Capability Matrix

**Specification Target:** GEMINI.md Section 7  
**Platform Target:** Bitaxe Ultra (Board 201) / ESP32-S3 / BM1366  
**Target Device:** `192.168.178.66` (Single authoritative physical device)  
**Status Hierarchy:** Implementation (PROVEN / PARTIALLY PROVEN / PLAUSIBLE / NOT PROVEN) | Test (TESTED / NOT TESTED) | Hardware (AVAILABLE / NOT AVAILABLE / UNKNOWN) | Measurement (MEASURED / NOT MEASURED)

---

## 1. Master Subsystem Capability Table

| Subsystem | Capability | Implementation File / Symbol | Test Coverage | Hardware Availability | Measurement Status | Truth Status |
|---|---|---|---|---|---|---|
| **Boot** | ESP32-S3 2nd-stage bootloader & app initialization | `main/main.c:app_main` | [TESTED] In-tree boot sequence verification | [AVAILABLE] ESP32-S3 dual-core LX7 @ 240MHz | [MEASURED] Boot sequence & partition table (`reports/post_upgrade_boot.log`) | **PROVEN** |
| **Wi-Fi** | 802.11 b/g/n STA station mode with WPA2/WPA3 support | `components/connect/connect.c:wifi_init_sta` | [TESTED] Connect event handlers & coexistence | [AVAILABLE] On-chip 2.4GHz Wi-Fi / BLE radio | [MEASURED] RSSI -67 dBm, connected to AP (`baseline_telemetry.json`) | **PROVEN** |
| **DHCP** | Non-destructive client renewal, event-driven acquisition | `components/connect/connect.c:ip_event_handler` | [TESTED] Non-destructive lease validation | [AVAILABLE] ESP-IDF LwIP DHCP client | [MEASURED] IP `192.168.178.66` assigned and verified | **PROVEN** |
| **DNS** | LwIP asynchronous DNS resolver for pool domain names | `components/connect/connect.c`, LwIP core | [TESTED] Pool FQDN resolution (`dgb.solopool.eu`) | [AVAILABLE] LwIP DNS client stack | [MEASURED] Stratum connection established via DNS resolution | **PROVEN** |
| **Network recovery** | Fast reconnection backoff without forced chip reboot | `components/connect/connect.c:connect_retry_timer_cb` | [TESTED] Reconnection state transition logic | [AVAILABLE] FreeRTOS software timer / ESP-NETIF | [MEASURED] Zero reboot loops across 33,000+ seconds uptime | **PROVEN** |
| **Stratum V1** | JSON-RPC 2.0 mining protocol with extranonce subscribe | `components/stratum/stratum_api.c`, `main/tasks/stratum_v1_task.c` | [TESTED] `components/stratum/test/test_mining.c` | [AVAILABLE] LwIP socket transport | [MEASURED] 3,511+ accepted shares on pool (`dgb.solopool.eu:3335`) | **PROVEN** |
| **Stratum V2** | Binary protocol with standard & extended channel support | `main/tasks/stratum_v2_task.c`, `components/stratum_v2/` | [TESTED] Frame serialization & noise protocol parsing | [AVAILABLE] secp256k1 & cryptographic coprocessor | [MEASURED] Protocol state machine operational | **PARTIALLY PROVEN** |
| **Pool failover** | Primary/Secondary pool automatic failover & fallback | `main/tasks/stratum_v1_task.c`, `main/system.c` | [TESTED] Fallback pool configuration parsing | [AVAILABLE] Dual pool configuration memory | [MEASURED] Active index tracked in system telemetry | **PROVEN** |
| **Job planning** | Bitcoin block header construction & midstate calculation | `components/stratum/mining.c:construct_bm_job` | [TESTED] `test_mining.c:Check coinbase tx`, `test_mining.c:Validate merkle` | [AVAILABLE] SHA256 hardware accelerator | [MEASURED] 3,000+ jobs constructed without hashing errors | **PROVEN** |
| **Work scheduling** | Bounded job queueing and interval timing | `main/tasks/create_jobs_task.c:create_jobs_task` | [TESTED] Job interval calculation from ASIC clock | [AVAILABLE] FreeRTOS queues and tasks | [MEASURED] ASIC job interval dynamically matched to hash core speed | **PROVEN** |
| **Nonce allocation** | Monotonic extranonce2 progression across job ticks | `main/tasks/create_jobs_task.c`, `components/stratum/mining.c` | [TESTED] `test_mining.c:Validate extranonce_2 uniqueness` | [AVAILABLE] Host-side work distribution | [MEASURED] Monotonic progression, zero duplicate work overlaps | **PROVEN** |
| **Version rolling** | BIP310 overt version rolling via BM1366 register `0xA4` | `components/asic/bm1366.c:BM1366_init` | [TESTED] `test_mining.c:Validate version mask incrementing` | [AVAILABLE] BM1366 overt version bitmask hardware | [MEASURED] Register 0xA4 set to `0x1FFFE000` (`reports/post_upgrade_boot.log`) | **PROVEN** |
| **Duplicate prevention** | Multi-attribute share submission deduplication cache | `main/tasks/asic_result_task.c:is_duplicate_share` | [TESTED] `test_mining.c:Validate extranonce_2 uniqueness` | [AVAILABLE] PSRAM / Internal RAM hash table | [MEASURED] 0 duplicate share rejections on pool | **PROVEN** |
| **Stale handling** | Generation tracking & clean jobs eviction on new blocks | `main/tasks/create_jobs_task.c`, `main/tasks/asic_result_task.c` | [TESTED] Job generation invalidation logic | [AVAILABLE] Atomic job snapshot array | [MEASURED] Low stale rate (18 stales / 3,529 total submissions = 0.51%) | **PROVEN** |
| **ASIC communication** | Full-duplex UART serial transport with BMXX framing | `components/asic/serial.c`, `components/asic/bm1366.c` | [TESTED] `components/asic/test/test_packet_validation.c` | [AVAILABLE] ESP32-S3 UART hardware controller | [MEASURED] Sustained transmission at 485MHz core clock | **PROVEN** |
| **ASIC discovery** | Chip detection, baud negotiation, and chip ID query | `components/asic/asic.c`, `components/asic/bm1366.c` | [TESTED] `test_packet_validation.c:Validate CRC5` | [AVAILABLE] BM1366 single-chip chain (Chain 0) | [MEASURED] Detected BM1366 on Board 201, 894 small cores | **PROVEN** |
| **ASIC result parsing** | 11-byte frame parsing, CRC5 validation, nonce extraction | `components/asic/bm1366.c:BM1366_process_work` | [TESTED] `test_packet_validation.c:Simulate ASIC response stream` | [AVAILABLE] Hardware UART RX ring buffer | [MEASURED] Zero framing errors under stable baud | **PROVEN** |
| **Midstate handling** | Double-SHA256 first-chunk midstate pre-computation | `components/stratum/mining.c:construct_bm_job` | [TESTED] `test_mining.c:Validate bm job construction` | [AVAILABLE] ESP32-S3 cryptographic hardware | [MEASURED] Cryptographic verification identical to cgminer reference | **PROVEN** |
| **Prehash handling** | Byte-reversed 32-bit word midstate ordering for BMXX | `components/stratum/mining.c:reverse_32bit_words` | [TESTED] `test_mining.c:Validate bm job construction` | [AVAILABLE] Word-swapping routines | [MEASURED] Match verified against hardware BM1366 requirements | **PROVEN** |
| **Thermal control** | Closed-loop temperature monitoring and target tracking | `main/thermal/thermal.c`, `main/thermal/EMC2101.c` | [TESTED] Sensor read & error handling logic | [AVAILABLE] Microchip EMC2101 I2C thermal monitor | [MEASURED] Device operating at stable 59°C target | **PROVEN** |
| **Fan control** | Closed-loop PWM fan control and tachometer feedback | `main/thermal/EMC2101.c:EMC2101_set_fan_duty` | [TESTED] Fan speed boundary clamps | [AVAILABLE] EMC2101 hardware PWM & tachometer | [MEASURED] Fan speed 29.9% duty, 3,592 RPM | **PROVEN** |
| **Voltage control** | Programmable core voltage regulator (DS4432U+ / TPS40305) | `main/power/vcore.c`, `main/power/asic_init.c` | [TESTED] Core voltage safety bounds (0.9V - 1.4V) | [AVAILABLE] Maxim DS4432U+ I2C DAC + TPS40305 buck | [MEASURED] VCore actual 1,206 mV at 1,200 mV setpoint | **PROVEN** |
| **Frequency control** | PLL register configuration & incremental clock stepping | `components/asic/pll.c`, `components/asic/bm1366.c` | [TESTED] `components/asic/test/test_pll.c` | [AVAILABLE] BM1366 on-chip PLL registers | [MEASURED] Actual frequency 485 MHz confirmed | **PROVEN** |
| **Auto tuning** | Efficiency-maximizing frequency/voltage search (J/TH) | `main/self_test/`, `main/power/` | [NOT TESTED] Static profiles utilized for stability | [AVAILABLE] Power telemetry + frequency control | [NOT MEASURED] Default manual profile active (485MHz / 1200mV) | **PLAUSIBLE** |
| **Memory management** | Dual-pool heap allocation (SRAM vs PSRAM caps) | `main/main.c`, `sdkconfig` | [TESTED] Allocation capability checks | [AVAILABLE] 512KB internal SRAM + 8MB Octal PSRAM | [MEASURED] Free internal RAM: 86KB, Free PSRAM: 7.56MB | **PROVEN** |
| **PSRAM** | High-capacity buffer placement for Web UI & JSON trees | `main/http_server/`, `main/display.c` | [TESTED] `esp_psram_is_initialized()` guards | [AVAILABLE] 8MB Octal SPI PSRAM | [MEASURED] `isPSRAMAvailable: 1`, 7,566,588 bytes free | **PROVEN** |
| **UI** | Modern Angular Axe-OS embedded single-page application | `main/http_server/axe-os/` | [TESTED] Headless Chrome Karma test suite (`test:ci`) | [AVAILABLE] Gzipped C arrays in flash | [MEASURED] Axe-OS served and rendered via HTTP server | **PROVEN** |
| **API** | OpenAPI 3.0 REST endpoints (`/api/system/info`, etc.) | `main/http_server/system_api_json.c`, `openapi.yaml` | [TESTED] JSON schema serialization checks | [AVAILABLE] ESP-IDF HTTP server component | [MEASURED] Full JSON payload response time < 15ms | **PROVEN** |
| **Telemetry** | Real-time structured telemetry (power, hashrate, heat) | `main/http_server/system_api_json.c`, `reports/` | [TESTED] Live data service & scoreboard validation | [AVAILABLE] INA260 I2C power monitor, EMC2101 sensor | [MEASURED] 12.44 W power, 435.93 GH/s hashrate, 28.5 J/TH | **PROVEN** |
| **OTA** | Dual-partition A/B fail-safe firmware updates via HTTP | `main/http_server/http_server.c`, `partitions.csv` | [TESTED] Partition verification & CRC checks | [AVAILABLE] 16MB Flash with `ota_0` and `ota_1` slots | [MEASURED] Successfully running on `ota_1` after verified OTA update | **PROVEN** |
| **Recovery** | Safe factory fallback partition & USB recovery path | `partitions.csv:factory`, `main/main.c` | [TESTED] Factory rollback triggers on consecutive boot fails | [AVAILABLE] Factory partition + ESP32-S3 USB JTAG/Serial | [MEASURED] Factory partition preserved at offset `0x20000` | **PROVEN** |
| **Watchdog** | Task watchdog (TWDT) & interrupt watchdog supervision | `main/main.c:app_main`, `sdkconfig` | [TESTED] Panic handler & reset reason logging | [AVAILABLE] Hardware timer watchdogs | [MEASURED] Reset reason: `Software reset via esp_restart` | **PROVEN** |

---

## 2. Hardware Capability Inventory (Bitaxe Ultra Board 201)

- **Compute Controller:** ESP32-S3 (Dual-core Xtensa LX7 @ 240 MHz, 512KB SRAM, 8MB Octal PSRAM, 16MB Quad SPI Flash)
- **ASIC Hash Unit:** Bitmain BM1366 (0.026 J/GH nominal, 894 small cores, 4 domain voltage distribution, overt ASICBoost)
- **Power Delivery:** TPS40305 Synchronous Buck Controller + DS4432U+ 7-bit I2C DAC for dynamic VCore adjustment (0.90V - 1.40V)
- **Power Telemetry:** Texas Instruments INA260 Precision Digital Current and Power Monitor over I2C
- **Thermal & Fan Control:** Microchip EMC2101 I2C Fan Controller with internal/external temperature diode & tachometer feedback
- **Storage & Layout:** 16MB Partition Table (NVS, Factory, OTA_0, OTA_1, Axe-OS WWW data)
- **Network Interface:** 802.11 b/g/n 2.4GHz with internal antenna, DNS resolver, dual Stratum pool failover
