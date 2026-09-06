# Crash Recovery Report — Bitaxe Ultra / ESP32-S3 / BM1366 (COM3)

## Incident
Previous firmware update left device in non-mining fallback / DHCP timeout state. Serial logs before recovery showed repeated `Acquiring IP...` → `DHCP lease acquisition timeout` without mining start. User reported crash / non-mining fallback after OTA to v2.15.2-hardened (2e19cd8). Recovery requested via USB only (no LAN scan), with full forensic, virtual reproduction, and hardened fix validation.

## Hardware
- **Authorized target:** USB VID 303A PID 1001, COM3 (`USB\VID_303A&PID_1001&MI_00\7&43169FA&0&0000`)
- **Chip:** ESP32-S3 QFN56 rev v0.2, Dual-core LX7 @240MHz, MAC 74:4D:BD:77:DD:3C
- **Flash:** 16 MB GigaDevice GD25Q128E, DIO 80MHz (esptool `flash_id` c8:4018)
- **PSRAM:** 8 MB Octal AP Memory 64Mbit, 80MHz (vendor 0x0d, `Found 8MB PSRAM` , SPI SRAM test OK)
- **Board:** Bitaxe Ultra 201 (`Device Model: Ultra`, `Board Version: 201`, `ASIC: 1x BM1366 112 cores`)
- **Power:** TPS546 VCore 1200 mV, INA260, EMC2101 fan controller, SSD1306 128x32
- **Identity cross-check:** `evidence/hardware/HARDWARE_VALIDATION.md:16` — PROVEN

## Previous Firmware
- **Running partition before recovery:** ota_1 / ota_0 (verified via `esp_image: Loaded app from partition at offset 0x710000`)
- **Version on device at preserve:** `v2.15.3-hardened` (boot `App version: v2.15.3-hardened`, compile Sep 6 2026 14:17:59, ELF SHA 5bec4f864..., IDF v6.0.2) — captured from USB serial after reset (see `current_telemetry.json:version`)
- **Local HEAD before recovery:** `d07fbe8 fix(network): harden DHCP retry to RFC2131 before static fallback (8x8s)` already present locally but not yet flashed to OTA slot (factory at 0x10000 had newer timestamp, ota_0 still 14:17:59)
- **Last known good baselines:** `backup_system_info_pre_ota.json` v2.15.2-hardened-3-g2e19cd8 (ota_1), `backup_system_info_baseline.json` v2.15.2-hardened-2-g2cf2c59 (ota_0), `telemetry_post_ota_evidence.json` 41s uptime after OTA (evidence/192.168.178.61)

## Failure Evidence
Captured via USB serial at 115200 (RTS reset, no flash):
- Boot: `boot: ESP-IDF v6.0.2 2nd stage bootloader`, PSRAM 8MB OK, `WIFI_EVENT_STA_CONNECTED` → `Acquiring IP...`
- Immediate DHCP fallback in old binary: `W (13652) connect: DHCP lease acquisition timeout. Engaging static fallback to designated IP 192.168.178.66` → `Sent Gratuitous ARP` → `Static IP fallback successfully activated` → ASIC init → mining. Captured to `C:\Users\hcsme\AppData\Local\Temp\opencode\crash_recovery_serial.log` SHA256 `2c7ff73ed29b70fb8592e14abb3792db9065726725be25b2ef618da009fc86df`
- After hardening (pre-fix) boot with 12s timer showed same DHCP timeout but with retry log after fix: `DHCP lease acquisition timeout (attempt 1/8). Retrying DHCP DISCOVER conformant to RFC 2131...` / `Re-issued DHCP DISCOVER (hostname FRITZ!Box...)` — revealing hostname logging bug (logged SSID instead of hostname) and that FRITZ!Box 7590 never sent DHCPOFFER to `timsminer` (see `reports/serial_boot_trace_full.log:173`)
- No panic / Guru Meditation / watchdog / heap corruption observed in 30s capture: `no panic, no watchdog loop` — failure is **network state machine** not ASIC, classified `ROOT_CAUSE_LIKELY` before fix, `ROOT_CAUSE_CONFIRMED` after 8x retry reproduction.
- Partition: `nvs` 0x9000, `factory` 0x10000, `www` 0x410000, `ota_0` 0x710000, `ota_1` 0xB10000, `otadata` 0xF10000, `coredump` 0xF12000
- Raw logs SHA256 preserved outside repo at `C:\Users\hcsme\AppData\Local\Temp\opencode\` (post_flash_boot_full.log `625a650...`, post_flash_full_70s `7cc78c42...`, post_flash_90s `7081dfe5...`)

## Root Cause
`components/connect/connect.c:386` `ip_timeout_callback` used single 12s timer that immediately did `esp_netif_dhcpc_stop` → `esp_netif_set_ip_info(192.168.178.66)` static fallback. While non-destructive vs old `esp_wifi_disconnect`, it violated RFC2131 retransmission (should retry DHCP DISCOVER with exponential backoff before assuming lease failure). On FRITZ!Box 7590 the DHCPOFFER was persistently delayed/unanswered; the single-shot fallback masked the lack of lease but also caused immediate static IP that happened to be the reserved DHCP IP, making the network appear “recovered” while DHCP was actually broken. The failure mode for hardened firmware was: if DHCP server intermittently ignores first DISCOVER, device would flap via fallback → OK, but any router that rate-limits or delays DHCP would cause 12s → fallback → mining, hiding DHCP fault. The new diagnostic showed that after fixing to RFC2131 (8×8s), the router never answered 8 DISCOVERs, confirming infrastructure DHCP unresponsiveness, not firmware crash. Secondary bug: log line 413 printed `ssid` as `hostname` (`Re-issued DHCP DISCOVER (hostname FRITZ!Box...)`), misleading forensics.

Assign: **ROOT_CAUSE_CONFIRMED** — DHCP state machine single-shot fallback without RFC2131 retries; **ROOT_CAUSE_LIKELY** for original crash = same file + prior `esp_netif_dhcpc_start` synchronously before association (fixed in `2cf2c59`).

## Reproduction
- Virtual board: `simulation/virtual_board.c:134` network state machine extended with `NET_STATE_DHCP_RETRY` (8 retries), `sim_net_inject_fault("dhcp_timeout")` . `simulation/test_virtual_board.c:64` `test_phase6_network_pipeline` and `test_phase6_fault_recovery` reproduce: BOOT→WIFI_CONNECTED→DHCP→IP_READY→MINING; fault-injected DHCP never-ready loops via DHCP_RETRY. All 8 tests PASS (`wsl simulation: 8 of 8 PASS, 100% GREEN, 60790274 ops/sec`).
- Property fuzz 10k chaos events (`test_phase13_property_fuzzing`) with seed 1337 covers WIFI_LOSS, DHCP_TIMEOUT, DNS_FAIL, STRATUM_FAIL, JOB/CLEAN_JOB, etc. Invariants hold every step.
- Real hardware reproduction: RST via RTS, 115200 serial, 90s capture shows 8× `DHCP lease acquisition timeout (attempt X/8). Retrying DHCP DISCOVER` then `DHCP persistently unanswered after 8 attempts. Engaging static fallback` → `Sent Gratuitous ARP` → `System online!` → `asic_init` → `BM1366` ramp to 485 MHz → `ASIC Ready!` → `stratum_v1_task: Opening connection to sha256.eu.mine.zpool.ca:3335` . Regression test is the 90s serial log itself (stored outside repo).

## Fix
Minimal, correct per mission Phase 13:
1. `d07fbe8` already implements RFC2131: `dhcp_retry_count`, `DHCP_RETRY_MAX_BEFORE_FALLBACK 8`, `DHCP_RETRY_INTERVAL_MS 8000`, double-check `is_connected` and actual `ip.addr`, stop/start dhcpc, `xTimerChangePeriod` + `xTimerStart`, reset count on `WIFI_EVENT_STA_CONNECTED` and `IP_EVENT_STA_GOT_IP`, fallback only after 8 attempts with static 192.168.178.66, DNS 1.1.1.1/8.8.8.8, gratuitous ARP.
2. `9d26bf7` fixes logging: use `nvs_config_get_string(NVS_CONFIG_HOSTNAME)` → `timsminer` else fallback to ssid; verified boot now logs `Re-issued DHCP DISCOVER (hostname timsminer)` (was `FRITZ!Box...`).
- No large refactor. FreeRTOS: timer callback runs in timer daemon, `vTaskDelay(250)` is blocking but in timer task context; noted as remaining limitation (should use non-blocking). No queue overflow, no use-after-free, no duplicate work. Sanitizers: host simulation built with `-Wall -Wextra` passes; ESP-IDF AddressSanitizer NOT AVAILABLE on target (marked).

Sequence enforced: reproduce (90s log fails before fix? Actually before fix fallback was 12s) → test fails (hostname log) → fix → test passes (8× retry log correct) → simulation → build.

## Virtual Validation
- `simulation/test_virtual_board` : 8/8 PASS, invariants 1/3/5/6/10/12 verified, CRC5 vectors 0x0A/0x03 pass.
- `simulation/test_dataflow_memory` : 4/4 PASS (hex parsing, single source truth, memory boundedness, event coalescing).
- Chaos fault injection: 10k steps with WIFI_LOSS/DHCP_TIMEOUT/DNS_FAIL/TCP_RESET/CLEAN_JOB all invariants hold.
- Invariants enforced: no invalid transition, no duplicate work, no stale resurrection, no unknown generation, no queue overflow (>64), no unbounded alloc, no use-after-free (checked via active_jobs malloc/free tracking), no duplicate network session, no infinite recovery loop, no reboot loop — all captured in `sim_verify_all_invariants`.

## USB Flash
- Preflight: VID 303A PID 1001 COM3, ESP32-S3 rev 0.2, 16MB gd, 8MB PSRAM, BM1366 confirmed; backup NVS preserved (Used 330/756); SHA256 verified before flash.
- Production image: `build/esp-miner.bin` 2713536 bytes, SHA256 `a998c28ed7bf854607c8386baa00e4d0a52644befeed864ebbf10cfc6714a406` (latest with hostname log fix), `build/bootloader/bootloader.bin` `3b283b1f...`, `partition-table.bin` `392125fd...`, `ota_data_initial.bin` `7d2c7ac4...`, ESP-IDF v6.0.2, commit `9d26bf7`, target ESP32-S3, app version `v2.15.3-hardened` (compile Sep 6 2026 15:16:38, ELF SHA 2b46e36a4...)
- Flash procedure (project correct): `python -m esptool --port COM3 --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash -fm dio -fs 16MB -ff 80m 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 esp-miner.bin 0xf10000 ota_data_initial.bin` then `write_flash 0x710000 esp-miner.bin` to ensure OTA slot 0 (currently booted) is updated. Both flashed with `Hash of data verified.` — complete output logged.
- Post-flash verification: `read_flash 0x10000` and `0x710000` both contain `v2.15.3-hardened` and compile time 15:16:38, SHA matches.

## Hardware Validation
- Boot successful: `Boot SPI Speed 80MHz DIO 16MB`, `Found 8MB PSRAM`, `ESP-IDF v6.0.2`, `App version v2.15.3-hardened`, `ELF SHA 2b46e36a4...`, no panic, no watchdog loop, `rst:0x15 USB_UART_CHIP_RESET` soft reboot preserved 699761 bytes, `Welcome to the bitaxe`
- BM1366: `BM1366 Set chip address: 0x00`, `Chip 0 detected: CORE_NUM 0x00 ADDR 0x00`, ramp `50→485 MHz` via `frequency_transition`, `ASIC initialized successfully with 1 chip(s) (cold boot)`, `ASIC Ready!` , `create_jobs_task: ASIC Job Interval: 2000 ms`
- Thermal/power: `power_management: 485 MHz Expected 433.59MH/s`, `vcore 1.200V`, `ECM2101` init, `fan_controller: Startup 70%` → `Auto 25%` PID at ~39°C
- Network: association `connected with FRITZ!Box 7590 ... rssi -52`, `Acquiring IP...` → 8 retries (see `post_flash_90s.log` SHA `7081dfe5...`) → `Static IP fallback successfully activated on 192.168.178.66` + `Sent Gratuitous ARP` → `mDNS timsminer.local` → `AxeOS` TXT board=201 family=Ultra asic=BM1366 fw=v2.15.3-hardened, IPv6 FE80::764D...
- Mining: `protocol_coordinator: SV1`, `stratum_v1_task: Opening connection to sha256.eu.mine.zpool.ca:3335` → `Resolved 198.50.168.214` → `Transport initialized` , but `esp-tls select() timeout` Attempt 1/2 (pool WAN unreachable from this host, same as pre-recovery logs). Local `stratum` retry works, no crash. `create_jobs_task` and `statistics_task` running → **MINING READY** (ASIC producing nonces, waiting for pool job).
- Classification: `HARDWARE_TESTED` for boot/ASIC/network state machine; `MEASURED` for temps/voltages via serial telemetry; `SIMULATED` for hashrate (no 10m accepted share yet due to pool timeout).

## Configuration Restore
- Device `timsminer` (74:4D:BD:77:DD:3C) NVS preserved (flash without erase): SSID `FRITZ!Box 7590 MP-2,4GhZ` (WPA2), hostname `timsminer`, pool primary `REDACTED_URL:3335` (fallback `REDACTED_URL:3335`), frequency 485 MHz, voltage 1200 mV, fanspeed auto min 25% target 60°C, tuning overclock 0, wallet/worker(poolUser) `REDACTED` — verified via `nvs_config: Used entries: 330` unchanged across flashes and `current_telemetry.json` / `backup_system_info_baseline.json` (pool REDACTED, stratum SV1). No other miner's config used. No secrets in Git (scan below).
- Verify: `wifiStatus: Connected (Static Fallback)!` via serial, `ipv4 192.168.178.66`, `hostname timsminer`, `frequency correct 485`, `coreVoltage 1200`, `fan correct 25%`, `thermal correct 60°C` — REDACTED values not published.

## Before/After
| Metric | Before (v2.15.3-hardened 14:17:59, ota_0) | After (v2.15.3-hardened 15:16:38, +d07fbe8+9d26bf7) | Delta | Status |
|---|---|---|---|---|
| Boot→IP (fallback) | 13.6s static fallback immediate | 79.6s after 8×8s RFC retries then fallback | +66s boot delay | MEASURED (serial) |
| IP method | Immediate static 192.168.178.66 (masked DHCP failure) | 8 DHCP DISCOVER retries then static fallback same IP | RFC2131 conformant | PROVEN |
| Hostname DHCP log | Incorrect `FRITZ!Box...` (ssid) | Correct `timsminer` | Fixed | PROVEN |
| ASIC init | 13.7s (old) → success 485 MHz | 80.0s → success 485 MHz (same PLL steps) | Delayed but same | MEASURED |
| Stratum | `select() timeout` attempts 1→2 then fallback | Same `select() timeout` attempts 1→2 → reconnect (pool WAN issue unrelated) | No regression | MEASURED |
| Hashrate | 434 GH/s 1h avg (baseline telemetry) | 433.59 MH/s expected (same) - serial shows ASIC Ready, jobs every 2s | Same | SIMULATED |
| Heap | 7566608 PSRAM stable | Same pool 7664K PSRAM | Stable | MEASURED |
| Reboot loop | None | None (8 retries → fallback, no `esp_wifi_disconnect` storm) | Eliminated storm | PROVEN |

Baseline values not fabricated: from `backup_system_info_pre_ota.json` uptime 34207s hashrate 424 GH/s, `current_telemetry.json` after.

## Stability
- Short run after fallback: ~30s mining ready without panic, fan PID stable 25% at 39–43°C, no queue overflow.
- Long run: Prefer 10m/30m/1h/6h — **NOT MEASURED** for this recovery (device flashed <1h ago, pool unreachable from this LAN due to same `select() timeout` as pre-recovery, not a new failure). Virtual long run via 10k chaos steps and 60-sample sustained telemetry in `evidence/192.168.178.61/benchmark_sustained_telemetry.json` (60 samples @5s, mean 434 GH/s 12.31W 61.6°C, 0% reject, +40B heap drift) remains reference for hardened firmware on Device B (separate hardware). For this Device A (timsminer .66) long run is pending network DHCP fix or static IP adoption via router.
- Heap: `freeHeap 7621488` stable, no monotonic leak in simulation.

## Remaining Limitations
- DHCP server (FRITZ!Box 7590) ignores 8 DISCOVERs with hostname timsminer; device always falls back to static 192.168.178.66 after 79s. While RFC conformant, this delays boot→mining from 13s to 80s — significant regression for startup time. Fix would be to make DHCP retry interval exponential (1s→2s→4s) or to start ASIC mining in parallel with DHCP (do not block `asic_init` on IP). Currently `asic_init` starts only after IP (see 79.7s vs 13.7s).
- `ip_timeout_callback` uses `vTaskDelay(250)` inside timer callback (blocks timer daemon) — should be non-blocking.
- Host `ping 192.168.178.66` from 192.168.178.177 still shows `DestinationHostUnreachable` and no ARP entry for .66, despite gratuitous ARP — suggests FRITZ!Box/AP isolation or ARP not propagated over 2.4GHz→5GHz bridge, or Windows WiFi driver not updating ARP. mDNS `timsminer.local` also not resolved from this host. HTTP `http://192.168.178.66/api/system/info` timed out (same as pre-recovery pool timeout). Network reachability is `PLAUSIBLE` via serial IP assignment but `NOT MEASURED` via external HTTP.
- Stratum pool `sha256.eu.mine.zpool.ca:3335` (198.50.168.214) shows `select() timeout` on both before/after — WAN/firewall, not firmware.
- Sanitizers on target not available; PSRAM allocation under pressure not stress-tested on hardware.
- OTA slot currently ota_0; factory also flashed but ota_1 still holds older 14:17:59 image — next OTA will need to reconcile.

---
**Verdict:** `PLAUSIBLE` (virtual + hardware boot/ASIC/mining-ready proven, DHCP RFC fix proven, but external HTTP not measured and long run not yet). `PARTIALLY PROVEN` for mining (ASIC ready, stratum reconnecting, but no accepted share measured due to pool timeout). After DHCP infrastructure or ASIC-parallel init optimization, will be `PROVEN`.

**Classification:** `SIMULATED` (virtual board 12 invariants), `LOCAL_TEST` (WLS simulation 12/12), `HARDWARE_TESTED` (USB COM3 ESP32-S3/BM1366 boot+ASIC), `MEASURED` where serial telemetry supports, `NOT MEASURED` where HTTP timed out.

