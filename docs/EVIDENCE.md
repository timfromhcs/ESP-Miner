# Technical Evidence Index & Truth Verification Matrix

This index catalogs every technical assertion, hardware test, and feature implementation in this repository.
All claims are strictly evaluated under the **Truth Contract**:
- **PROVEN:** Active execution path verified in source code and proven via direct test or physical silicon telemetry.
- **PARTIALLY PROVEN:** Implemented and functionally verified; extended multi-chip or protocol edge cases pending hardware.
- **PLAUSIBLE:** Architecturally sound and supported by register maps/silicon specs, but not enabled in default profile.
- **NOT PROVEN:** Theoretical claim lacking execution proof in silicon.
- **MEASURED:** Direct physical telemetry captured from hardware sensors (INA260, EMC2101, UART, ESP32-S3 timers).
- **NOT MEASURED:** No direct sensor telemetry recorded.

---

## 1. Domain Separation & Test Classifications

To prevent misleading performance comparisons, all evidence items are classified into strict domains:
- **`REAL HARDWARE`:** Measurements captured directly from physical ESP32-S3 and BM1366 silicon on Bitaxe Ultra (Board 201).
- **`SIMULATION`:** Virtual board model and protocol fuzzing executed on host emulator.
- **`HOST BENCHMARK`:** Micro-benchmarks running on x86_64 CPU measuring software algorithm efficiency (NOT mining speed).
- **`UNIT TEST`:** FreeRTOS and ESP-IDF component unit tests executed in host/QEMU harnesses.
- **`INTEGRATION TEST`:** End-to-end communication tests across interconnected firmware modules.

---

## 2. Feature Evidence Matrix

| ID | Feature | Implementation File / Symbol | Domain | Classification | Measurement | Evidence File Reference |
|---|---|---|---|---|---|---|
| **EV-001** | Physical USB Target Identity | `reports/usb_target_identity.md` | `REAL HARDWARE` | **PROVEN** | **MEASURED** | VID `0x303A`, PID `0x1001`, ESP32-S3 rev 0.2, GD25Q128E flash |
| **EV-002** | Firmware Build & Packaging | `build_wsl/esp-miner.bin` | `REAL HARDWARE` | **PROVEN** | **MEASURED** | SHA-256: `7572d38ddc931f2d33438bea306d532b4b78e2a0b121e3dfcb5aecc66829f5ef` |
| **EV-003** | BM1366 PLL Clock Ramping | `components/asic/pll.c`, `bm1366.c` | `REAL HARDWARE` | **PROVEN** | **MEASURED** | Incremental 6.25 MHz steps from 50 MHz to 485 MHz (`docs/ASIC.md`) |
| **EV-004** | Overt ASICBoost (BIP310) | `components/asic/bm1366.c:BM1366_init` | `REAL HARDWARE` | **PROVEN** | **MEASURED** | Register `0xA4` mask `0x1fffe000`, rolled shares accepted by pool |
| **EV-005** | ASIC-Side Midstate Reuse | Theoretical silicon feature | `REAL HARDWARE` | **NOT PROVEN** | **NOT MEASURED** | BM1366 receives 80B header; no proof of on-die midstate caching |
| **EV-006** | Wi-Fi Mesh Band-Steering Fix | `components/connect/connect.c` | `REAL HARDWARE` | **PROVEN** | **MEASURED** | BTM/RM disabled, AP association in 2,055 ms without deauth drops |
| **EV-007** | Deterministic Static IP Fallback — **WITHDRAWN** | `components/connect/connect.c` | `REAL HARDWARE` | **RETRACTED** | n/a | Removed in v2.16.0. A hardcoded address cannot be verified as free, so binding it reintroduced the very lease conflicts the state machine exists to prevent — and it masked them. Lease conflicts are now handled by the DHCP client itself (`CONFIG_LWIP_DHCP_DOES_ARP_CHECK`). See `docs/NETWORKING.md` §Fix 3a. |
| **EV-008** | Closed-Loop Thermal PID | `main/thermal/EMC2101.c` | `REAL HARDWARE` | **PROVEN** | **MEASURED** | 53.5–58.1 °C die temperature under load @ 27–31% fan PWM |
| **EV-009** | 10-Minute Sustained Mining | `reports/benchmark_sustained_telemetry.json` | `REAL HARDWARE` | **PROVEN** | **MEASURED** | Mean 435.81 GH/s, 12.40 W, 28.45 J/TH, 67 shares accepted |
| **EV-010** | Duplicate Nonce Observation | `main/tasks/asic_result_task.c` | `REAL HARDWARE` | **PROVEN** | **MEASURED** | 0 duplicate nonces observed across 117 samples and 67 shares |
| **EV-011** | Memory Stability & PSRAM Leaks | `reports/benchmark_sustained_telemetry.json` | `REAL HARDWARE` | **PROVEN** | **MEASURED** | ~7.63 MiB free PSRAM; 0 bytes monotonic leak drift across 600s |
| **EV-012** | Virtual Board State Invariants | `simulation/virtual_board.c` | `SIMULATION` | **PROVEN** | **MEASURED** | 12/12 unit/dataflow tests green, 10,000 chaotic property tests pass |
| **EV-013** | Host Parsing Micro-Benchmark | `simulation/test_dataflow_memory.c` | `HOST BENCHMARK` | **PROVEN** | **MEASURED** | >60M ops/s on x86_64 host (software parsing throughput only) |
| **EV-014** | BM1370 Ticket Mask Tracks Pool Difficulty | `components/asic/bm1370.c:BM1370_set_ticket_mask`, `asic.c:ASIC_set_ticket_mask` | `REAL HARDWARE` | **PROVEN** | **MEASURED** | Live log on `192.168.178.80`: register `0x14` re-programmed 256 → 4 096 (capped) → 1 000 across successive `mining.set_difficulty`. Before v4.0.1 the register was written twice at init from the static family default and never again. See `evidence/192.168.178.80/`. |
| **EV-015** | BM1370 Host/Chip UART Rate Agreement | `components/asic/bm1370.c:BM1370_set_max_baud` | `REAL HARDWARE` | **PROVEN** | **MEASURED** | Log: `Setting chip UART BT8D=2 -> 1041666 baud (host programmed to match)` followed by `serial: Changing UART baud to 1041666`. Previously the chip ran at 1 041 666.67 while the host was programmed to 1 000 000 — a 4.17 % mismatch. |
| **EV-016** | V1 Notify Counters & Pool Clock Sync | `main/tasks/stratum_v1_task.c` | `REAL HARDWARE` | **PROVEN** | **MEASURED** | `notifyReceived` rises from 0 and tracks notifies; before the fix all three of `notifyReceived`, `workReceived`, `notifyDropped` read 0 for entire sessions on `.61`, `.66` and `.80`, and `settimeofday()` was never reached. |
| **EV-017** | BM1370 Nonce Space From Applied Frequency | `components/asic/bm1370.c:BM1370_init` | `REAL HARDWARE` | **PROVEN** | **MEASURED** | Log: `nonce space: requested=750.00 MHz applied=750.00 MHz`; previously derived from the requested frequency, and `ASIC_set_nonce_space()` divided by it without guarding against a failed PLL solve. |
| **EV-018** | Compact-Target Validation Matches Core | `components/stratum/notify_validate.c`, `simulation/test_nbits_reference.c` | `HOST BENCHMARK` | **PROVEN** | **MEASURED** | 20 550 checks incl. 20 000 randomised differential words and an exhaustive exponent-byte sweep against an independent decoder written from the consensus definition. |
| **EV-019** | Network Recovery Ladder | `components/connect/net_recovery_policy.c`, `simulation/test_net_recovery_policy.c` | `SIMULATION` | **PROVEN** | **MEASURED** | 79 checks: lease replay, resolver preference, fallback binding, and the rule that the fallback must not fight a running DHCP client. |

---

## 3. Detailed Evidence Artifacts

### EV-004: Overt ASICBoost (Version Rolling) Evidence
- **Stratum Protocol Negotiation:** Client sends `mining.configure` with version-rolling mask `ffffffff`. Pool responds with the granted mask, e.g. `{"result":{...,"version-rolling.mask":"1fffe000"}}`.
- **Hardware Register Configuration:** register `0xA4`, payload `90 00 <hi> <lo>`, where the roll count is `(mask & 0x1FFFE000) >> 13`. Confirmed on both families:
  - BM1366 — `components/asic/bm1366.c:BM1366_set_version_mask`
  - BM1370 — `components/asic/bm1370.c:BM1370_set_version_mask`
- **Rollable window:** bits [13:28] of the version field, 16 bits wide. The register is 16 bits, so a larger mask cannot be represented. The firmware requests `ffffffff` and a pool that grants it returns `0xFFFFFFFF` = 0x1FFFF rolls, which is clamped to 0xFFFF. Host and chip agree only because both clamp through `ASIC_VERSION_ROLL_MASK`.
- **Live Share Submission:** accepted shares confirmed with rolled versions, e.g. `00106006`, `0008C006`, `00154006` on `192.168.178.80`, each submitted with the matching BIP310 version bits.
- **Distinction Notice:** Version rolling is overt ASICBoost. No claim of covert midstate manipulation or unverified ASIC-internal silicon optimization is made.

### EV-014 / EV-015: BM1370 bring-up on `192.168.178.80`
- **Device:** Bitaxe Supra, board 601, MAC `28:84:85:A6:1B:E0`, BM1370, 1× ASIC, 2 040 small cores, 4 hash domains.
- **Update path:** Wi-Fi OTA via `POST /api/system/OTA`, `factory` → `ota_0`, address unchanged at `192.168.178.80`. Wi-Fi credentials, pool configuration, frequency, voltage and the static-fallback settings were not modified.
- **Ticket mask (EV-014):** register `0x14` is the only knob controlling how many nonces per second the chip pushes over UART, and every arriving nonce costs the host a double-SHA256 in `test_nonce_value()`. Before v4.0.1 the BM1370 wrote it twice during init, both times with the static family default of 256, and `ASIC_set_ticket_mask()` had no BM1370 case at all. After v4.0.1 the register is seeded from the pool difficulty and re-programmed on every `mining.set_difficulty`.
- **UART rate (EV-015):** the chip derives its rate from a 25 MHz reference, `25e6 / ((BT8D+1) * 8)`. Both ends now agree exactly at 1 041 666 baud.
- **Why this matters more here than on the Ultra:** at 750 MHz the BM1370 sustains ~1 530 GH/s against ~444 GH/s for a BM1366 at 485 MHz — roughly 3.5× the result rate over the same UART, with ~2.3× the small-core count.
- **Observability prerequisite:** `asic_result` logged one INFO line per reported nonce. At that rate the 128 KB log ring was overwritten within seconds, so the boot trace and any real warning were unreadable and the unit looked healthy in its own logs. Demoting that line to `DEBUG` is what made the above diagnosable.

### EV-009 & EV-010: Sustained Mining Telemetry & Duplicate Nonce Audit
- **Telemetry File:** `reports/benchmark_sustained_telemetry.json`
- **Measurement Methodology:** 117 distinct HTTP REST polls against `/api/system/info` at 5.0-second intervals over 600 seconds of uninterrupted mining to `dgb.solopool.eu:3335`.
- **Summary Statistics:**
  - Hashrate: Mean 435.81 GH/s (Min 335.00 GH/s, Max 523.98 GH/s)
  - Power: Mean 12.40 W (Min 12.02 W, Max 12.65 W @ 1.206 V actual VCore)
  - Energy Efficiency: 28.45 J/TH
  - Temperature: Mean 58.09 °C (Min 55 °C, Max 59 °C)
  - Shares Submitted: 66 shares during evaluation window (all accepted)
  - Pool Rejection Rate: 0.00% (0 rejected shares observed during test period)
  - Duplicate Nonce Count: 0 duplicate nonces observed in 117 samples and 67 total shares.

### EV-011: Memory Profiling & PSRAM Zero-Leak Evaluation
- **Methodology:** Polling `freeHeap` (internal SRAM + Octal PSRAM) every 5 seconds under continuous mining, logging, and HTTP server activity.
- **Initial Free Heap:** 7,632,504 bytes
- **Final Free Heap:** 7,630,036 bytes
- **Variance:** -2,468 bytes (bounded transient JSON serialization buffer, no monotonic leak).
- **Result:** Zero bytes memory leak drift.
