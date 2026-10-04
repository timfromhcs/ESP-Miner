# ESP-Miner (Hardening & Physical Validation Edition)

ESP-Miner is the open-source firmware powering the Bitaxe family of Bitcoin ASIC miners, running on an ESP32-S3 dual-core microcontroller and driving Bitmain BM13xx ASIC hashing engines.

This edition is the result of an exhaustive, evidence-based autonomous engineering and hardening cycle conducted directly on physical silicon (Bitaxe Ultra Board 201) under the strict **Truth Contract**. Every claim in this repository is classified by verification state: **PROVEN**, **PARTIALLY PROVEN**, **PLAUSIBLE**, or **NOT PROVEN**, backed by raw telemetry and reproducible tests.

---

## Current Status

- **Hardware Validation:** `HARDWARE VALIDATED`
  - **`timsminer` (`192.168.178.61`, MAC `74:4D:BD:77:DD:3C`):** verified on physical Bitaxe Ultra Board 201, ESP32-S3 rev 0.2, BM1366 ASIC. Flashed over USB (COM3), then **3/3 clean cold boots** on `v4.0.0-stable-timsminer` (single `rst:0x` per cycle, no panic, no stack overflow). Mining 412 GH/s at 485 MHz, 52 shares accepted, **0 rejected**.
  - **`blackharkminer` (`192.168.178.66`, MAC `74:4D:BD:77:99:80`):** verified on a second physical Bitaxe Ultra Board 201. Upgraded over **Wi-Fi OTA** (`POST /api/system/OTA`) from `v3.0.0-stable-blackharkminer` to `v4.0.0-stable-blackharkminer`. Mining 419 GH/s at 485 MHz, 20 shares accepted, **0 rejected**.
  - Note the identity change: earlier revisions of this file listed `.66` as "Device A" and `.61` as "Device B". The addresses are bound to the hostnames above.
- **Memory Stability:** `HARDWARE VALIDATED` — two independent defects that both caused reboot loops were found, fixed and verified. See [docs/MEMORY_STRATEGY_PLAN.md](docs/MEMORY_STRATEGY_PLAN.md).
- **Simulation Validation:** `SIMULATED` (Virtual board model plus four dependency-free host binaries — `test_virtual_board`, `test_dataflow_memory`, `test_notify_validate`, `test_power_policy` — all green under `-Wall -Wextra -Werror`, and 10,000 chaotic property tests passing).
- **CI / Automated Testing:** `PASSING` (Headless Karma/Angular tests, ESP-IDF compile checks, QEMU Unity tests, host test suite).
- **Release Status:** `STABLE PRODUCTION RELEASE` — Version **v4.0.0-stable**. The `general` variant carries no baked-in address; `timsminer` and `blackharkminer` are per-device variants.
- **Known Issue (open):** the DHCP re-assert watchdog in `components/connect/connect.c` can re-apply the fallback address while a valid DHCP lease is already up, which produces two brief IP flaps shortly after boot and can delay pool DNS resolution by ~30 s. It is self-healing and does not stop mining, but it should only re-assert when the interface has actually lost its address.
- **Plan & Evidence:** [`docs/MEMORY_STRATEGY_PLAN.md`](docs/MEMORY_STRATEGY_PLAN.md) · [`docs/PLAN_V2_17_STALE_AND_LIMITS.md`](docs/PLAN_V2_17_STALE_AND_LIMITS.md) · [`evidence/192.168.178.66/`](evidence/192.168.178.66/README.md)

---

## Verified Hardware

| Hardware Component | Specification | Verification Status |
|---|---|---|
| **Board Model** | Bitaxe Ultra (Board Version: 201) | `HARDWARE VALIDATED` |
| **Microcontroller** | ESP32-S3 (QFN56 revision v0.2), Dual-Core Xtensa LX7 @ 240 MHz | `HARDWARE VALIDATED` |
| **ASIC Hashing Unit** | 1× Bitmain BM1366 (112 core clusters, 894 small hashing engines) | `HARDWARE VALIDATED` |
| **Flash Memory** | 16 MB SPI Flash (GigaDevice GD25Q128E, Quad-SPI @ 80 MHz) | `HARDWARE VALIDATED` |
| **PSRAM** | 8 MB Octal SPI PSRAM (AP Memory 64 Mbit @ 80 MHz) | `HARDWARE VALIDATED` |
| **Power Management** | TPS40305 Synchronous Buck + Maxim DS4432U+ I2C DAC (1.200 V VCore) | `HARDWARE VALIDATED` |
| **Thermal & Fan Control** | Microchip EMC2101 I2C Thermal Monitor & PWM Controller | `HARDWARE VALIDATED` |
| **Power Telemetry** | Texas Instruments INA260 Precision Digital Power Monitor | `HARDWARE VALIDATED` |
| **Display** | SSD1306 (128×32 OLED over I2C) | `HARDWARE VALIDATED` |

---

## Key Improvements

1. **Retracted Claim — Mesh Band-Steering Lockout:** this file previously claimed that 802.11v/k band steering on AVM FRITZ!Box routers was withholding DHCP offers, and that disabling it was the fix. That diagnosis was **wrong** and has been retracted; the observed outages were caused by the memory defect described in item 12. The earlier access-point, ARP-conflict and `sent=0` explanations were also wrong — see [docs/PLAN_V2_17_STALE_AND_LIMITS.md](docs/PLAN_V2_17_STALE_AND_LIMITS.md). Kept here so the retracted reasoning is not silently reused.
2. **Deterministic DHCP Client Lifecycle:** DHCP client start is bound to 802.11 station association (`WIFI_EVENT_STA_CONNECTED`) and only issued when the client is actually stopped — no `stop`→`start` bounce, which resets the DHCP xid. A valid lease is never wiped on re-association, retries use jittered backoff, and the station re-associates after three exhausted cycles until a real lease arrives. Lease conflicts are handled by the DHCP client itself via `CONFIG_LWIP_DHCP_DOES_ARP_CHECK`, which is deliberately *not* ACD, because ESP-IDF documents that ACD "can cause repeated DHCP DECLINEs" on access points that echo the client's MAC in ARP replies — exactly what a FRITZ!Box does. See [docs/NETWORKING.md](docs/NETWORKING.md). **Correction to an earlier claim in this file:** there is no longer "no static-IP fallback" — item 3 describes the mechanism that is actually implemented, and the `general` release keeps it disabled with no address baked in.
3. **Correct, Operator-Configured Static Fallback:** if DHCP genuinely fails there is an optional last-resort static IPv4 address - stored per-device in NVS, **off by default with no address baked in**, editable in AxeOS, and applied only after the retry/recovery cycles are exhausted. There is deliberately no site-specific default: a default naming a specific address would make every unit built from this firmware claim an address it was never given, which is precisely how duplicate-IP conflicts start. Enable it per device for an address reserved on that device's router. The earlier hardcoded version was removed for the same reason, and the bug that made it unreachable (setting the IP while the netif was down, so it never reached LwIP) is fixed and now asserted at runtime.
4. **Undecodable Jobs Are No Longer Mined:** A `mining.notify` frame whose coinbase cannot be decoded is now dropped *before* it is enqueued. Previously the chip burned a full ~6.7 s job interval on work that could never yield an acceptable share. A new dependency-free validator (`components/stratum/notify_validate.c`) checks job id, prev-block hash, both coinbase halves, every merkle branch and version/nbits/ntime before anything is allocated, and `hex2bin()`'s return value is finally checked — a short merkle branch used to leave uninitialised heap on the chip.
5. **Captive DNS Port 53 Isolation:** Constrained captive DNS server strictly to active SoftAP mode, preventing UDP port 53 contention in station mode.
6. **Stratum V1 IPv4-First Resolution:** Configured `hints.ai_family = AF_INET` before `AF_UNSPEC`, eliminating 2–5s IPv6 AAAA record timeout delays on IPv4-only mining pools.
7. **Proactive Thermal Management (ATM):** A hysteresis buffer with frequency *and* voltage moving together, a regulator floor tied to the TPS546/TPS40305 rather than to the current setting, anti-hunting "DPS memory", a minimum dwell between changes, and a fan-relief path. Derates are held in memory only — NVS stays the single source of truth for the user's setting — and the shutdown latch is now clearable after a real cooldown.
8. **Opt-in V/F Autotuner:** Holds frequency and minimises voltage behind an acceptance gate (≥95 % of expected hashrate, ≤0.1 % errors), reverting to the last proven-stable voltage on failure and aborting above the board's 65 °C continuous limit.
9. **Pool Health Observability:** `notifyReceived` / `notifyDropped` and `sharesRejectedStale` / `sharesRejectedOther` are exposed in the API and on the home screen, so the unavoidable stale-share latency race can finally be told apart from a real client defect. Previously both were a single opaque `sharesRejected` number.
10. **Smooth PLL Clock Stepping:** Implemented incremental 6.25 MHz PLL ramping from 50 MHz to 485 MHz to prevent TPS40305 core voltage droop during cold boot.
11. **Closed-Loop Thermal PID:** Fine-tuned EMC2101 PID loop ($K_p=5.0, K_i=0.1, K_d=2.0$) maintaining BM1366 die at 53–58 °C under 27–31% fan PWM.
12. **Internal DRAM Headroom & Overload Protection:** two independent defects produced identical reboot loops and both are fixed and verified on hardware.
    - *Defect 1 — silent 512 KB buffer in internal DRAM.* `main/log_buffer.c` declares its log ring as `EXT_RAM_NOINIT_ATTR`. That attribute is **conditional**: unless `CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY` is set, ESP-IDF silently degrades it to `__NOINIT_ATTR` and the buffer lands in internal `.noinit`. The ESP32-S3 caps `dram0_0_seg` at `0x54700` (341 KB), so a 512 KB buffer overflowed it and the link failed with `region 'dram0_0_seg' overflowed by 363232 bytes`. Because `sdkconfig` is not versioned, a locally drifted `sdkconfig` reproduced this on *every* commit and looked like a code regression. `main/CMakeLists.txt` now aborts the build with an explicit message if either PSRAM segment option is not `y`, and `sdkconfig.defaults` documents why. Result: internal DRAM reserve went from **−363 KB to +176 KB**.
    - *Defect 2 — idle task stack.* `CONFIG_FREERTOS_IDLE_TASK_STACKSIZE` was at the IDF default of 1536 B. On this board Wi-Fi coexistence, the task WDT and the scheduler run in the idle context, and the canary was overrun reproducibly at ~2.3 s into boot (`***ERROR*** A stack overflow in task IDLE1 has been detected.`, then `rst:0xc` in a loop). Raised to 4096 B.
    - *Right-sizing.* The log ring went from 512 KB to 128 KB. It is fully zeroed and `esp_cache_msync`ed on every cold boot, so the larger buffer was pure boot latency for memory that is rarely read. 128 KB still fits the internal reserve, which makes the build survive even a misconfigured `sdkconfig` instead of failing to link.
    - *Overload protection.* New `main/memory_guard.c` watches the **largest free internal block**, not total free heap, because FreeRTOS allocates task stacks as one contiguous block — 40 KB of fragmented free heap is as unusable for a 4 KB stack as an empty heap, and that is precisely how the `IDLE1` panics arose. It warns below 24 KB free, and only after 3 consecutive critical samples (15 s hysteresis) does it log a full diagnostic and perform a controlled `esp_restart()` instead of an unexplained panic. Cost is two `heap_caps` calls every 5 s and it is not in the ASIC path.
    - Full analysis, measurements and the rejected hypotheses (LVGL, NimBLE, data cache) are in [docs/MEMORY_STRATEGY_PLAN.md](docs/MEMORY_STRATEGY_PLAN.md).

---

## Real Measurements

All measurements below were captured from physical hardware sensors (INA260, EMC2101, UART, and LwIP socket telemetry):

### 10-Minute Sustained Mining Benchmark (`reports/benchmark_sustained_telemetry.json`)
- **Evaluation Duration:** 600.0 seconds (117 continuous telemetry samples @ 5s interval).
- **Mean Sustained Hashrate:** **435.81 GH/s** (Min: 335.00 GH/s, Max: 523.98 GH/s).
- **Power Consumption:** **12.40 W mean** (Min: 12.02 W, Max: 12.65 W @ 1.206 V actual VCore).
- **Energy Efficiency:** **28.45 J/TH**.
- **Die Operating Temperature:** **58.09 °C mean** (Min: 55 °C, Max: 59 °C, setpoint 60.0 °C).
- **Pool Share Acceptance:** **66 shares submitted and accepted** during the 10-minute run.
- **Observed Rejections / Duplicates:** **0 rejects, 0 stale submissions, 0 duplicate nonces observed**.
- **Memory Stability:** Initial Free Heap: 7,632,504 bytes | Final Free Heap: 7,630,036 bytes (variance: 2.4 KiB for JSON serialization buffer; **0 bytes memory leak drift**).

### Device B Sustained Telemetry Benchmark (`evidence/192.168.178.61/benchmark_sustained_telemetry.json`)
- **Evaluation Duration:** 300.0 seconds (60 continuous telemetry samples @ 5s interval).
- **Mean Sustained Hashrate:** **434.22 GH/s** (Min: 356.20 GH/s, Max: 515.39 GH/s).
- **Power Consumption:** **12.31 W mean** (Min: 12.10 W, Max: 12.53 W).
- **Energy Efficiency:** **28.35 J/TH**.
- **Die Operating Temperature:** **61.6 °C mean** (setpoint 62.0 °C).
- **Pool Share Acceptance:** **37 shares submitted and accepted**, **0 rejections** (0.00% rejection rate).
- **Previous Rejections on v2.14.0:** 253 rejections due to "Invalid job id" (100% resolved on v2.15.3-hardened).
- **Memory Stability:** Initial: 7,632,208 bytes | Final: 7,632,248 bytes (**0 bytes monotonic leak drift**).

### Cold Boot Startup Pipeline Latencies (`reports/serial_boot_trace_full.log`)
- $T_0 	o T_1$ (Reset to App Main Init): **1,079 ms**
- $T_1 	o T_2$ (Wi-Fi AP Association): **976 ms** (2,055 ms total)
- $T_2 	o T_3$ (DHCP IPv4 Acquisition): **1,654 ms** (3,709 ms total)
- $T_3 	o T_4$ (ASIC Init & PLL Ramp 50 $	o$ 485 MHz): **8,257 ms** (11,966 ms total)
- $T_4 	o T_5$ (Stratum DNS Resolution & Socket Handshake): **1,828 ms** (13,794 ms total)
- $T_5 	o T_6$ (First Job Dequeued to BM1366): **273 ms** (14,067 ms total)
- $T_0 	o T_7$ (Cold Boot to First Valid Share Accepted): **15,177 ms** (~15.2 seconds).

---

## Architecture

```
┌────────────────────────────────────────────────────────────────────────┐
│                              ESP32-S3                                 │
│                                                                        │
│  ┌──────────────────────┐             ┌─────────────────────────────┐  │
│  │   Axe-OS Web App     │             │     Stratum V1 / V2 Client  │  │
│  │   (Angular 19 SPA)   │             │   - JSON-RPC / Noise Protocol│ │
│  └──────────┬───────────┘             └──────────────┬──────────────┘  │
│             │ HTTP REST / WebSocket                  │ Jobs            │
│  ┌──────────▼───────────┐             ┌──────────────▼──────────────┐  │
│  │     HTTP Server      │             │      Create Jobs Task       │  │
│  │  - Static ROM assets │             │   - Coinbase tx generation  │  │
│  │  - JSON API endpoints│             │   - Hardware Merkle calc    │  │
│  └──────────────────────┘             │   - Midstate precalc        │  │
│                                       └──────────────┬──────────────┘  │
│                                                      │ 1.04 MBaud UART    │
│  ┌──────────────────────┐             ┌──────────────▼──────────────┐  │
│  │  EMC2101 Thermal PID │             │      BM1366 ASIC Driver     │  │
│  │  - Closed-loop PWM   │             │   - BIP310 Version Rolling  │  │
│  │  - 500ms sample rate │             │   - CRC5 Packet Verification│  │
│  └──────────┬───────────┘             └──────────────┬──────────────┘  │
└─────────────┼────────────────────────────────────────┼─────────────────┘
              │ I2C                                    │ UART
┌─────────────▼───────────┐             ┌──────────────▼──────────────┐
│  EMC2101 / INA260 / DAC │             │      1× BM1366 ASIC         │
│  (Thermal & Power Stage)│             │      (112 Cores @ 485 MHz)  │
└─────────────────────────┘             └─────────────────────────────┘
```

Detailed architectural breakdowns:
- [System Architecture](docs/ARCHITECTURE.md)
- [Networking & Protocol Engine](docs/NETWORKING.md)
- [BM1366 ASIC Subsystem](docs/ASIC.md)
- [Job Planning & Merkle Tree Subsystem](docs/JOB_PLANNING.md)
- [Memory Management & PSRAM](docs/MEMORY.md)
- [Performance & Benchmarks](docs/PERFORMANCE.md)
- [Axe-OS UI/UX Specification](docs/UI_UX.md)

---

## Build

### Prerequisites
- Linux (Ubuntu 22.04 LTS / WSL2)
- ESP-IDF **v6.0.2** (`. ~/esp/esp-idf/export.sh`)
- Node.js **v22.x LTS** and npm

### Compilation
```bash
# 1. Source ESP-IDF environment
. ~/esp/esp-idf/export.sh

# 2. Build firmware (automatically compiles Axe-OS frontend and static C arrays)
idf.py build
```

---

## Test

### Automated Firmware Unit & Integration Tests
```bash
# Run host simulation & dataflow invariants
cd simulation && make test

# Run C firmware unit tests
idf.py build test

# Run Axe-OS Angular frontend tests (Headless Chrome CI)
cd main/http_server/axe-os
npm run test:ci
```

---

## Flash / OTA

### USB Flash (Primary & Recovery Channel)
To flash the firmware directly via the onboard USB Serial/JTAG port:
```bash
python -m esptool --port COM3 --baud 921600 write_flash 0x710000 build/esp-miner.bin
```

### Fail-Safe Dual-Partition Layout
The device uses standard ESP-IDF dual OTA partitions (`ota_0` at `0x710000`, `ota_1` at `0xB10000`) and a dedicated factory recovery image (`factory` at `0x10000`). If an OTA image fails to boot or validate, the bootloader automatically reverts to the previous operational partition.

---

## Evidence & Truth Classification

Every feature and measurement is cataloged in the [Evidence Matrix](docs/EVIDENCE.md) and [Capability Matrix](docs/CAPABILITY_MATRIX.md).

| Claim | Evidence ID | Classification | Proof Method |
|---|---|---|---|
| **Cold Boot Pipeline (<16s)** | `EV-001` | **PROVEN** | Physical boot trace capture |
| **Wi-Fi Band-Steering Fix** | `EV-006` | **PROVEN** | Continuous FRITZ!Box STA association |
| **Static IP Fallback + Gratuitous ARP** | `EV-007` | **PROVEN** | RFC 5227 broadcast & ARP table update |
| **BM1366 485 MHz @ 1.200 V** | `EV-003` | **PROVEN** | INA260 power telemetry & PLL readback |
| **BIP310 Overt ASICBoost** | `EV-004` | **PROVEN** | Register 0xA4 mask & pool share logs |
| **Closed-Loop Thermal PID** | `EV-008` | **PROVEN** | Continuous EMC2101 diode telemetry |
| **Zero Memory Drift in Telemetry** | `EV-011` | **PROVEN** | 10-minute sustained heap monitoring |
| **ASIC-Side Midstate Reuse** | `EV-005` | **NOT PROVEN** | Unconfirmed in silicon; 80-byte header retransmitted |

---

## Limitations

1. **Single-ASIC Architecture:** Multi-chip chaining and distributed work balancing are not applicable to Bitaxe Ultra (Board 201), which is a single-chip BM1366 platform.
2. **Frequency Boundaries:** Operating frequencies above 525 MHz or core voltages above 1.300 V exceed the thermal dissipation capacity of the standard 40mm heatsink and risk electromigration.
3. **RF Coexistence:** Active BLE advertising during initial Wi-Fi connection can cause packet collision on the single 2.4 GHz radio; BLE is strictly deferred until station association is complete.

---

## Known Experimental Areas

- **Stratum V2 Binary Channels:** Basic framing and handshake are implemented, but extended channel multiplexing remains in active development.
- **Automatic Multi-Profile Auto-Tuning:** Dynamic frequency-voltage hill-climbing exists in prototype form (`main/self_test/`), but manual static profiling (485 MHz / 1200 mV) remains recommended for long-term stability.

---

## Security

- **Network Access Control:** `is_network_allowed()` restricts administrative API access to RFC 1918 private IPv4 subnets and matching local hostnames (`timsminer.local`).
- **Secret Redaction:** Wi-Fi pre-shared keys, Stratum pool passwords, and private configuration data are stored in encrypted/protected NVS on-chip and are strictly redacted from public telemetry and logs.

---

## Development & Contributing

- Master Autonomous Engineering Specification: [`GEMINI.md`](GEMINI.md)
- Development Guide: [`AGENTS.md`](AGENTS.md)
- Full Technical Changelog: [`CHANGELOG.md`](CHANGELOG.md)
- Final Engineering Report: [`docs/FINAL_ENGINEERING_REPORT.md`](docs/FINAL_ENGINEERING_REPORT.md)

---

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.
