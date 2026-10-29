# Baseline — v4.0.2-rc1 candidate

Recorded 2026-10-29 before any source change (Phase 0 / Phase 1 of `GEMINI.md`).
Raw evidence is private (`artifacts/private/…`, git-ignored); this file is the
redacted summary. Redacted: SSID, MAC, pool user/worker, payout address.

## Source

| Item | Value |
|---|---|
| Branch at start | `release/v4.5.0` (identical to `master` / `origin/master`) |
| HEAD at start | `a7426273c470291dd452acc3b258e5136b290fa5` = tag `v4.0.1-stable` |
| Worktree at start | only `GEMINI.md` modified (operator's spec, preserved, not committed) |
| Last Stable | `v4.0.1-stable` (GitHub release 2026-10-19), CI for that SHA: Build Firmware ✔, Unit Test ✔ (78 QEMU tests, 0 failures), Release ✔, Factory ✔ |
| Submodule | `components/libsecp256k1/libsecp256k1` @ `0cdc758a` (v0.6.0) |

The branch name `release/v4.5.0` does not match the version this work produced.
The diff is backwards-compatible fixes only, so it is a patch release
(`v4.0.2`); work continues on `release/v4.0.2`.

## Toolchain

| Tool | Version |
|---|---|
| ESP-IDF | v6.0.2 (WSL Ubuntu, `~/esp/esp-idf`) |
| xtensa gcc | esp-15.2.0_20251204 |
| Host gcc | 15.2.0 (Ubuntu) |
| Python | 3.14.4 (WSL), 3.14.6 (Windows) |
| Node / npm | v22.23.3 (WSL), v22.23.1 / 10.9.8 (Windows); CI uses 24.14.0 |
| QEMU | 9.2.2 esp_develop_9.2.2_20250817 (installed during this work) |

## Configuration facts

| Item | Value | Label |
|---|---|---|
| Partition table | factory 4 MiB @0x10000, www 3 MiB, ota_0 / ota_1 4 MiB each, otadata, coredump | PROVEN |
| Rollback | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` not set | PROVEN |
| Static fallback, general build | off, no address | PROVEN |
| Static fallback, `tim2` build | on, address = the unit's router reservation; STA MAC and BSSID pinned | PROVEN (`tools/build_release.sh`) |

## Target `192.168.178.80` — identity

| Field | Value |
|---|---|
| `boardVersion` | `601` ✔ |
| `ASICModel` | `BM1370` ✔ (`/api/system/asic` reports `deviceModel: Gamma`; board 601 = `FAMILY_GAMMA` in `main/device_config.h` — earlier docs wrongly called it "Bitaxe Supra") |
| `version` | `v4.0.1-stable-tim2` |
| `idfVersion` | v6.0.2 |
| `runningPartition` | `ota_0` (factory: v4.0.0-stable-tim2, ota_1: v4.0.1-stable-tim2) |
| Station MAC | matches the `tim2` build pin ✔ |
| `apEnabled` | 0 (station mode) |
| `resetReason` | power-on |
| `uptimeSeconds` | 656 268 (7.6 d) |

## Target — passive health window

`tools/collect_telemetry.ps1`, 20 samples every 30 s over 571.7 s,
2026-10-29 20:01–20:11 local. Thresholds were not set for the baseline; it is a
reference, not a pass/fail.

| Metric | Value |
|---|---|
| Resets during window | 0 (uptime monotonic) |
| hashrate 1 h mean | 1 526.9 GH/s (expected 1 530) |
| hashrate 10 m | 1 520.6 – 1 527.7 GH/s |
| Shares accepted | +101 → 10.6 / min |
| Shares rejected | +1 (session total: 50 — 49 "Invalid job id", 1 "Low difficulty share") |
| Stale | +1 (session total 341) |
| Pool | SV1, no TLS, not on fallback, pool difficulty 2 092 |
| ASIC temp | 59.6 – 60.3 °C |
| VR temp | 81 – 82 °C |
| Power | 28.76 – 28.91 W → 18.88 J/TH |
| Core voltage | 1 200 mV set, 1 197 mV measured |
| Frequency | 750 MHz (above the 690 MHz top of `frequencyOptions`; `overclockEnabled` = 0) |
| Fan | ~47 %, ~5 360 RPM, auto |
| Internal free heap | 71 627 – 71 895 B, no downward trend |
| PSRAM free | ~7.93 MB |
| Largest internal block | 31 744 B |
| `notifyDropped` | 0 of 41 789 |
| Wi-Fi RSSI | −71 dBm |
| CPU | 17 % |

### Anomalies noted (not acted on)

- 750 MHz is outside the listed frequency options while `overclockEnabled` is 0.
  Unchanged; frequency/voltage are out of scope for this release.
- VR temperature ~82 °C is the hottest reading on the board.
- 49 "Invalid job id" rejects over the session (0.35 % of accepted). Not
  investigated in this release.

## Raw evidence (private)

| File | SHA-256 |
|---|---|
| `info.json` | `44a41dfb…a992` |
| `statistics.json` | `06495623…e03e` |
| `asic.json` | `88db10f4…abae` |
| `scoreboard.json` | `24170684…a7f9` |
| `baseline_window.csv` | `90073997…ed27` |
| Known-good image `esp-miner-v4.0.1-stable-tim2.bin` (= device `ota_0`: same version string and build timestamp) | `4bd7bdbaf787df957cd04a7a404d9ba48d602e8f22513d7f2b0aaa1472432461` |
