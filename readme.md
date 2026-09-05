#ESP-Miner (Hardened Edition)

Open-source firmware for the **Bitaxe** series of Bitcoin ASIC miners, built on the ESP-IDF framework (v6.0.2) for the ESP32-S3 and featuring the AxeOS Angular web interface.

---

## Operational Status

|| Component | Status | Verification |
||---|---|---|
|| **Build & Toolchain** | **REPRODUCIBLE** | ESP-IDF v6.0.2 + Node v22 |
|| **Hardware Mining** | **PROVEN (MEASURED)** | Bitaxe Ultra (Board 201), BM1366 @ 485 MHz / 1200 mV |
|| **Overt ASICBoost** | **PROVEN (MEASURED)** | Version rolling active, valid shares accepted by pool |
|| **Wi-Fi / DHCP** | **HARDENED** | Non-destructive DHCP recovery implemented & tested |
|| **Thermal PID** | **PROVEN (MEASURED)**| 44.9 C - 46.4 C steady state @ setpoint 60 C |

---

## Supported Hardware

- **Bitaxe Ultra** (Board 201, BM1366 ASIC, 112 cores)
- **Bitaxe Max** (Board 101/102, BM1397 ASIC)
- **Bitaxe Supra** (Board 401, BM1368 ASIC)
- **Bitaxe Gamma** (Board 601, BM1070 ASIC)

---

## Key Proven Features

- **Direct BM11366 Control:** Hardware SPI communication with per-core status reporting and accurate HCN calculations.
- **Overt ASICBoost (Version Rolling):** Stratum negotiation of bitmask mapped into BM11366 version rolling registers (0xA4), verified with live pool share acceptance.
- **Robust Networking:** Non-destructive DHCP discovery retry loop that eliminates link flapping on APs with heavy 2.4GHz coexistence.
- **Closed-Loop Thermal Management:** EMC2101/EMC2103 fan control via PID algorithm preserving safe operating temperatures.
- **Self-Contained Web UI (AxeOS):** Angular SPA bundled, gzipped, and embedded directly into flash.

---

## Building from Source

- ESP-IDF v6.0.2
- Node.js v22.x & npm 10.x

```bash
. ~/esp/esp-idf/export.sh
idf.py build
```

---

## Documentation & Evidence

- [Architecture Overview](docs/ARCHITECTURE.md)
- [Build Instructions](docs/BUILD.md)
- [Flashing Guide](docs/FLASHING.md)
- [Technical Evidence Matrix](docs/EVIDENCE.md)
- [Hardware-in-the-Loop Validation](evidence/hardware/HARDUARE_VALIDATION.md)
- [Testing Strategy](docs/TESTING.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
