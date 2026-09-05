# ESP-Miner (Hardening & Evidence Edition)

[![Build Status](https://img.shields.io/badge/build-ESP--IDF%20v6.0.2-brightgreen)](docs/BUILD.md)
[![Release](https://img.shields.io/badge/release-v2.15.2--hardened-blue)](https://github.com/timfromhcs/ESP-Miner/releases/tag/v2.15.2-hardened)
[![Truth Contract](https://img.shields.io/badge/Truth_Contract-Strict-orange)](docs/EVIDENCE.md)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

---

## What This Project Is

ESP-Miner (Hardening & Evidence Edition) is an open-source, engineering-focused firmware built for the **Bitaxe** series of Bitcoin ASIC miners (ESP32-S3 SoC powering Bitmain BM1366x ASICs).

This fork prioritizes **Truth-Contract Engineering**: Every assertion, feature, and capability is classified as `either PROVEN, PARTIALLY PROVEN, PLAUSIBLE or NOT PROVEN`, with actual runtime evidence recorded from physical hardware on COM3. No marketing claims, fake benchmarks, or unconfirmed silicon capabilities are presented as fact.


---


## Feature & Verification Status Matrix


| Feature | Implementation | Verification | Measurement | Reference |
|---|---|---|---|---|
|**Overt ASICBoost (Version Rolling)** | Implemented | **PROVEN** | **MEASURED** | Register 0xA4 configured, rolled shares accepted by pool [docs/EVIDEiCE.md](docs/EVIDENCE.md) |
|**ASIC-Side Midstate Reuse** | Unknown | **NOT PROVEN** | **NOT MEASURED** | BM1366 receives 80-byte headers, no silicon-internal reuse proof |
|**Wi-Fi / DHCP Hardening** | Implemented | **PROVEN** | **MEASURED** | Non-destructive DHCP retry elasticity, zero flap [logs](docs/HARDWARE_VALIDATION.md) |
|**Closed-Loop Thermal PID** | Implemented | **PROVEN** | **MEASURED** | 44.9 C - 46.4 C @ 25-30% PWM [docs/HARDUARE_VALIDATION.md](docs/HARDWARE_VALIDATION.md) |
|**Stratum V1 Client** | Implemented | **PROVEN** | **MEASURED** | 17.0 - 39.2 ms latency, receives `mining.notifya |
|**Deterministic Work Splitting** | Partial | **PARTIALLY PROVEN** | **MEASURED** | Extranonce2 incremented; multi-ASIC split not applicable on Ultra |
|**Duplicate-Free Nonce Scheduling** | Partial | **PARTIALLY PROVEN** | **NOT MEASURED** | Stale queues flushed; internal BM1366 core traversal is opaque |

---

## Warranty & Truth Contract

- **Version Rolling is Overt ASICBoost:** We prove that version-rolling is successfully negotiated with the pool and programmed into BM1366 register 0xA4. We **do not** claim covert ASICBoost or ASIC-internal midstate caching without silicon verification.
- **No Fabricated Hashrates:** Hashrates are reported based on actual PLL clocking (485 MHz) and live difficulty-accepted shares.

---


## Supported Hardware

- **Bitaxe Ultra** (Board 201, Bitmain BM1366 ASIC, 112 Cores) * (Physically Tested & Verified)*
- **Bitaxe Max** (Board 101/102, BM1397)
- **Bitaxe Supra** (Board 401, BM1368)
- **Bitaxe Gamma** (Board 601, BM1370)

---


## Physical Hardware Validation (COM3)

- We have validated this build directly on a Bitaxe Ultra via USB Serial (COM3):
  - ASIC Cores: 112 cores initialized and mapped.
  - Voltage & Frequency: 485 MHz @ 1200 mV (1.20V).
  - Temperature & Fan: 44.9 C - 46.4 C (PID output 25-30%).
  - Share Submission: Shares accepted by `github.com/timfromhcs`'s pool network with latency ranging from 17.0 to 39.2 ms.

---

## Building From Source

ensure you have ESP-IDF v6.0.2 and Node.js v22 x installed:

```bash
. ~/esp/esp-idf/export.sh
idf.py build
```

---

## Flashing via USB (Only)

To ensure safety and compliance with rules, network flashing is not provided. Flash via direct physical USB serial:
```bash
python -m esptool --port COM3 --baud 460800 write_flash 0x10000 build/esp-miner.bin
```

---

## Documentation Index

- [Architecture & Mining Pipeline](docs/ARCHITECTURE.md)
- [Build & Toolchain Reproducibility](docs/BUILD.md)
- [USB Flashing & Safety Guide](docs/FLASHING.md)
- [Technical Evidence & Verification Matrix](docs/EVIDENCE.md)
- [Physical Hardware-In-The-Loop Validation](docs/HARDWARE_VALIDATION.md)
- [Testing Strategy & QEMU](docs/TESTING.md)
- [Release Provenance & Checksums](docs/RELEASE.md)
- [Troubleshooting & Recovery](docs/TROUBLESHOOTING.md)
- [Changelog](CHANGELOG.md)

---

## License
This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
