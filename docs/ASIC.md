# ASIC Subsystem Specification & Physical Validation

**Status:** PROVEN | **Measurement:** MEASURED  
**Target ASICs:** 1x Bitmain BM1366 (Board 201) and 1x Bitmain BM1370 (Board 601)  
**Operating Points:** BM1366 485 MHz @ 1.200 V VCore · BM1370 750 MHz @ 1.195 V VCore

---

## 1. ASIC Silicon Architecture & Features

### BM1366 — Bitaxe Ultra (Board 201)
- **Hashing Capacity:** ~0.89 GH/s per MHz across all cores (~433–440 GH/s nominal at 485 MHz).
- **Topology:** 112 core clusters, 894 small hashing engines, 4 hash domains.
- **Voltage Distribution:** 4 internal voltage domains fed by TPS40305 synchronous buck converter.
- **Clock Synthesis:** On-chip dual-PLL with fractional feedback dividers.

### BM1370 — Bitaxe Supra (Board 601)
- **Hashing Capacity:** ~2.04 GH/s per MHz across all cores (1 530 GH/s nominal at 750 MHz).
- **Topology:** 128 core clusters, 2 040 small hashing engines, 4 hash domains.
- **Clock Synthesis:** dual-PLL, solved over a 160–239 feedback-divider range (the BM1366 uses 144–235).

### Overt ASICBoost (BIP310) — both families
Hardware register `0xA4` enables on-chip version rolling. The chip sweeps the
rollable version bits itself and returns the bits it used in the result frame,
so the host never has to re-send a job per version.

The rollable window is **bits [13:28] of the version field** — 16 bits. Bits
above 28 are BIP9 signalling and are not rollable. The register is also only 16
bits wide, so the roll count is `(mask >> 13) & 0xFFFF` and a mask that asks for
more is clamped, not truncated silently. `ASIC_VERSION_ROLL_MASK` in
`components/asic/include/asic_common.h` is the single definition of that window.

This matters because the firmware itself requests `"version-rolling.mask":
"ffffffff"` in `mining.configure`. Taken literally that is 0x1FFFF rolls, which
does not fit; the chip receives 0xFFFF. Host and chip therefore agree only
because both clamp to the same window.

---

## 2. Communications & Protocol Framing

### Host-to-ASIC Transport
- **Physical Interface:** ESP32-S3 UART hardware controller (TX GPIO 43, RX GPIO 44).
- **Baud Rate:** Programmed to the **exact** rate the chip derives from its 25 MHz
  reference, `baud = 25 000 000 / ((BT8D + 1) * 8)`. With BT8D = 2 that is
  **1 041 666 baud**.

  Both families previously programmed the chip with BT8D = 2 but returned
  1 000 000 to the host — a systematic 4.17 % mismatch in which the receiver
  samples the wrong bit grid. This was found and fixed on the BM1366 first; the
  BM1370 kept the defect until v4.0.1, which matters far more there because the
  Supra produces roughly 2.5x the result rate over the same link.

- **Packet Structure (Host TX):**
  - BMXX Header (2 bytes: `0xAA 0x55`)
  - Command Type / Job Payload (76 bytes: Midstate 32B, Merkle Root remainder 12B, nTime 4B, nBits 4B, Version 4B)
  - CRC5 Integrity Checksum (5 bits)
- **Packet Structure (ASIC RX):**
  - Frame Header (1 byte: `0xAA`)
  - Work ID / Job Identifier (2 bytes)
  - Nonce (4 bytes, little-endian)
  - Version Bits (2 bytes rolled version)
  - CRC5 Checksum (1 byte)

### Hardware Framing Integrity
- CRC5 validation is implemented in hardware/assembly in the `bm13xx.c` drivers.
- **Physical Measurement:** 0 framing errors and 0 CRC mismatches observed across over 100,000 processed work responses.

### Register 0x14 — the ticket mask
This is the single knob that sets how much UART traffic a given hashrate
produces: the chip forwards a nonce only when the (reversed) hash satisfies the
mask, so a mask of N means roughly `hashrate / N` nonces per second on the wire.
Every reported nonce costs the ESP32 a full double-SHA256 in `test_nonce_value()`
before it is either submitted or discarded, so an under-set mask costs CPU, not
just log volume.

| Family | Before v4.0.1 | After v4.0.1 |
|---|---|---|
| BM1366 | seeded from pool difficulty, re-programmed on every `mining.set_difficulty` | unchanged |
| BM1370 | written twice at init from the static family default (256) and never again | seeded from pool difficulty, re-programmed on every `mining.set_difficulty` |

On a Supra the old behaviour asked a ~1.5 TH/s chip for one report per 256
hashes — several million per second across a 1 Mbaud link. The static default was
inherited from the much slower Ultra, and the driver had no runtime hook at all.

---

## 3. Dynamic PLL Ramping & Power Safety

Cold-booting a high-frequency ASIC directly at target frequency causes a steep
di/dt transient that trips the regulator's overcurrent protection or dips Vcore,
causing brownouts.

### Safe Transition Algorithm
The ESP-Miner firmware implements incremental PLL clock ramping:
1. **Boot Frequency:** ASIC initializes at a safe baseline of 50.00 MHz.
2. **Core Voltage:** Maxim DS4432U+ 7-bit I2C DAC programs the TPS40305 feedback voltage.
3. **Step Ramping:** frequency ascends in 6.25 MHz increments every 100 ms.
4. **Baud Switch:** upon reaching the target clock, the UART switches to the exact rate from §2.

If no valid PLL solution exists for the requested frequency, register `0x08` is
**not** written. A zeroed divider set leaves the PLL unlocked and the chip idle
until a reboot, which is strictly worse than staying at the previous frequency.
For the same reason the nonce space (HCN) is sized from
`POWER_MANAGEMENT_MODULE.actual_frequency` — the clock the chip actually runs at
— with a fallback to the requested value and a logged mismatch when they differ.

---

## 4. Closed-Loop Thermal & Fan Control

- **Sensor Hardware:** Microchip EMC2101 I2C Fan Controller with an external diode sensing the ASIC substrate.
- **Thermal Setpoint:** 60.0 °C (overheat protection trigger: 75.0 °C).
- **Controller:** closed-loop PID (Kp = 5.0, Ki = 0.1, Kd = 2.0) updating at 500 ms intervals.
- **Measured Thermal State (BM1366):**
  - Ambient: 22.4 °C
  - Die temperature under full load: 53.0 °C – 55.0 °C
  - Fan duty: 27.0 % – 31.5 % PWM
  - Fan acoustic profile: whisper quiet (<30 dBA)
