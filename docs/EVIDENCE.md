# Technical Evidence Index & Verification Matrix

This index catalogs every technical assertion, hardware test, and feature implementation in this repository.
All claims are evaluated under the strict **Truth Contract**:
- **PROVEN:** Implementation exists, active execution path traced, and verified via test or real hardware evidence.
- **PARTIALLY PROVEN:** Code implementation exists and partially verified; edge cases remain unverified.
- **PLAUSIBLE:** Architecturally possible but lacks direct verification.
- **NOT PROVEN:** Claimed or theoretical but missing active caller-callee execution path or silicon proof.
- **MEASURED:** Direct physical hardware measurement conducted and recorded.
- **NOT MEASURED:** No direct measurement performed.

---

## 1. Feature Evidence Matrix

| ID | Feature | Implementation Status | Verification | Measurement | Primary Evidence / Reference |
|---|---|---|---|---|---|
|**EV-001** | **Physical USB Hardware Target** | Implemented | **PROVEN** | **MEASURED** | reports/usb_target_identity.md |
|**EV-002** | **Firmware Build & Packaging** | Implemented | **PROVEN** | **MEASURED** | docs/BUILD.md, build/esp-miner.bin |
|**EV-003** | **BM1366 ASIC Initialization & PLL\* | Implemented | **PROVEN** | **MEASURED** | docs/HARDWARE_VALIDATION.md (485 MHz @ 1.20V) |
|**EV-004** | **Overt ASICBoost (Version Rolling)** | Implemented | **PROVEN** | **MEASURED** | Stratum negotiation (0x001fff00), register 0xA4, accepted shares |
|**EV-005** | **ASIC-Side Midstate Reuse** | Theoretical | **NOT PROVEN** | **NOT MEASURED** | BM1366 SPI payload retransmits 80-byte header; no on-chip reuse proof |
|**EV-006** | **Non-Destructive Wi-Fi DHCP Recovery** | Implemented | **PROVEN** | **MEASURED** | components/connect/connect.c, prevents link flap |
|**EV-007** | **Closed-Loop Thermal PID Fan Control** | Implemented | **PROVEN** | **MEASURED** | Steady 44.9 C - 46.4 C at 25-30% PWM (Setpoint 60.0 C) |
|**EV-008** | **Stratum V1 Mining & Share Acceptance** | Implemented | **PROVEN** | **MEASURED** | Latency 17.0 - 39.2 ms, zero rejects on live pool |
|**EV-009** | **Deterministic Work Splitting** | Partial | **PARTIALLY PROVEN** | **MEASURED** | Extranonce2 progression; 1-ASIC Ultra not applicable for multi-ASIC |
|**EV-010** | **Duplicate Nonce Prevention** | Partial | **PARTIALLY PROVEN** | **NOT MEASURED** | Queue flushes on clean_jobs=true; silicon core traversal is opaque |

---

## 2. Technical Evidence Artifacts

### EV-001: Physical Hardware Target
- **Target:** Bitaxe Ultra (Board 201)
- **Controller:** ESP32-S3 (revision v0.2), 16MB Flash, 8MB Octal PSRAM
- **ASIC:** 1x Bitmain BM1366 (112 cores)
- **Serial Connection:** Direct physical USB (COM3, VID 0x303A, PID 0x1001)

### EV-004: Overt ASICBoost (Version Rolling) Evidence
- **Stratum Protocol Negotiation:** Client sends mining.configure with version-rolling mask 001fff00.
- **Hardware Register Configuration:** BM1366 driver configures version rolling mask via register 0xA4 (components/asic/bm1366.c:489).
- **Live Share Submission:** Accepted shares confirmed with versions such as 201B8202, 200CA202, and 2017C202.
- **Distinction Notice:** Version rolling is overt ASICBoost. No claim of covert midstate manipulation or unverified ASIC-internal silicon optimization is made.

### EV-006: Wi-Fi / DHCP Reliability Hardening
- **Root Cause Identified:** Factory firmware started DHCP client synchronously before 802.11 association and dropped Wi-Fi link after 30 seconds of pending DHCP.
- **Hardening Fix:** Deferred DHCP client initialization to WIFI_EVENT_STA_CONNECTED and replaced destructive disconnects with periodic non-destructive discovery restarts.
- **Result:** Miner successfully acquires IP without link flapping in congested 2.4 GHz environments.
