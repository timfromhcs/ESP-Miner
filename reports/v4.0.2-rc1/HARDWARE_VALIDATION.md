# Hardware Validation — v4.0.2-rc1

Target: `http://192.168.178.80` — the only authorized device (`GEMINI.md` §0.2).

## 1. Acceptance thresholds (declared 2026-10-29 20:40, before any post-flash sample)

Baseline reference: `BASELINE.md` (v4.0.1-stable-tim2, 1h hashrate 1 526.9 GH/s).

| # | Criterion | Smoke (first 10 min) | Sustained (60 min) |
|---|---|---|---|
| T1 | Identity | board 601, BM1370, version `v4.0.2-rc1-tim2`, partition `ota_1` | same, every sample |
| T2 | No reset | uptime monotonic | uptime monotonic |
| T3 | Network | `wifiStatus` connected, `apEnabled` 0, IPv4 `192.168.178.80` | same, every sample |
| T4 | Pool | ≥ 1 accepted share | ≥ 200 accepted shares (baseline rate ~10.6/min ⇒ ~640 expected) |
| T5 | Reject ratio | — | (rejected Δ) / (accepted Δ) ≤ 2 % |
| T6 | Hashrate | `hashRate_10m` at end ≥ 1 374 GH/s (90 % of baseline) | mean `hashRate_10m` over last 30 min ≥ 1 374 GH/s |
| T7 | Thermal | ASIC ≤ 65 °C, VR ≤ 90 °C, `overheat_mode` 0 | same |
| T8 | Memory | internal free heap ≥ 60 000 B | internal free heap ≥ 60 000 B; last-15-min mean ≥ 95 % of first-15-min mean |
| T9 | Work flow | `notifyDropped` Δ = 0 | `notifyDropped` Δ = 0 |

A missing sample is reported, not interpolated. Physical router-outage recovery
is out of scope for this run (`NOT MEASURED`, `GEMINI.md` §19.3).

## 2. OTA execution

| Item | Value |
|---|---|
| Source commit | `ee4dd9a` (firmware sources identical at `96c3781`) |
| Built | clean clone in WSL, `tools/build_release.sh tim2`, ESP-IDF v6.0.2 |
| Image | `esp-miner-tim2.bin`, 2 755 248 B, SHA-256 `3c74028b5b00828947147b471837e94747a64c479cf9136d5905d04200083f35` |
| esptool `image-info` | ESP32-S3 (chip id 9), checksum valid, validation hash valid, project `esp-miner`, version `v4.0.2-rc1-tim2` |
| Pre-flash check (20:38:26) | board 601, BM1370, `v4.0.1-stable-tim2`, `ota_0`, uptime 658 507 s, AP off, 59.9 °C / VR 82 °C, heap 71 979 B, MAC == tim2 pin |
| Upload | one attempt, 20:38:26 → 20:38:44 (17 s), curl exit 0, HTTP 200 `Firmware update complete, rebooting now!` |
| Return | first poll after 2 s: no answer; second poll (20:38:54, ~10 s after the response): answered |
| Post-flash identity | board 601, BM1370, `v4.0.2-rc1-tim2`, `ota_1`, uptime 0, reset reason `Software reset via esp_restart`, Wi-Fi connected, AP off, IPv4 `192.168.178.80` |
| Previous image | `v4.0.1-stable-tim2` remains on `ota_0` (switch back via `POST /api/system/boot`) |
| Rollback | not enabled — not claimed |

## 3. Results

_Filled in after the observation window; see section 4._
