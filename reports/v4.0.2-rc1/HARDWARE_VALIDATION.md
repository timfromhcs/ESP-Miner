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

## 3. Results — `192.168.178.80` (Board 601 / BM1370)

Window: 124 samples over 3 711.6 s (every 30 s via `tools/collect_telemetry.ps1`),
2026-10-29 ~20:40–21:42, no gap longer than 45 s. Evaluated from the raw CSV
(private), not from the tool's summary.

| # | Criterion | Result | Verdict |
|---|---|---|---|
| T1 | Identity | all 124 samples: `v4.0.2-rc1-tim2`, `ota_1`. Board 601 / BM1370 were checked in the pre-flash, post-flash and final snapshots, **not per sample** (not a CSV column) | PASS (partly per-snapshot) |
| T2 | No reset | uptime strictly increasing, 32 s → 3 744 s | PASS |
| T3 | Network | every one of 124 polls answered at `192.168.178.80`; final snapshot: Wi-Fi connected, `apEnabled` 0; RSSI −77…−67 dBm. Wi-Fi state was **not** a CSV column | PASS (inferred from reachability + final snapshot) |
| T4 | Shares | smoke: 110 accepted by ~600 s; sustained: +533 (threshold 200) | PASS |
| T5 | Reject ratio | rejected Δ 0, stale Δ 0, other Δ 0 → 0 % (threshold ≤ 2 %) | PASS |
| T6 | Hashrate | `hashRate_10m` at ~600 s: 1 533.5 GH/s; mean over last 30 min: 1 530.8 GH/s; minimum after warm-up 1 526.5 GH/s (threshold 1 374) | PASS |
| T7 | Thermal | ASIC max 60.4 °C, VR max 82 °C, `overheat_mode` 0 | PASS |
| T8 | Memory | internal heap min 78 311 B (threshold 60 000); last-15-min mean / first-15-min mean = 0.998; PSRAM min 7.92 MB | PASS |
| T9 | Work flow | `notifyDropped` Δ 0, `notifyReceived` +293 | PASS |

### Against the baseline (`BASELINE.md`, same unit, v4.0.1)

| Metric | v4.0.1 baseline (9.5 min) | v4.0.2-rc1 (61.9 min) |
|---|---|---|
| hashrate 1 h mean | 1 526.9 GH/s | 1 533.5 GH/s |
| power | 28.83 W | 28.81 W |
| J/TH | 18.88 | 18.79 |
| ASIC / VR temp | 60.0 / 81.5 °C | 60.0 / 81.1 °C |
| internal heap | 71.7 kB | 78.7 kB (higher, because the unit had just booted; no downward trend) |
| rejected + stale | +2 in 9.5 min | 0 in 61.9 min |
| accepted shares / min | 10.6 | 8.6 |

The share rate is lower because the pool's variable difficulty sat higher in
this window (1 000 → 4 406, mean 2 756, vs. a constant 2 092 in the baseline). At
the same hashrate a higher share difficulty means fewer, larger shares.
`PLAUSIBLE`, not isolated by a controlled test. The differences in hashrate,
power and temperature are within normal run-to-run variation; this run
establishes **no improvement**, only that nothing regressed.

### What this does and does not show

- `HARDWARE VALIDATED` for the criteria above on one unit for one hour. Not a
  multi-day soak.
- The image was uploaded **through the previous firmware's OTA handler**
  (`v4.0.1`). The new handler in this RC has therefore **not** run on hardware.
- Wi-Fi reconnect after an outage (the headline fix) was **not** exercised:
  `NOT MEASURED`.
- Rollback is not enabled and was not tested.

## 4. Other units flashed in the same session (BM1366, Board 201)

Not part of the declared acceptance run (those thresholds were written for the
Board 601 unit). Recorded because one of them failed.

| Unit | Result |
|---|---|
| `blackharkminer` (`192.168.178.66`, MAC ends `99:80`) | OTA of the variant **without a BSSID pin**: HTTP 200, then **no answer for 6.4 min** (8 polls in the bounded window, then stopped; no second upload sent). The unit was later found on USB (`COM4`), its flash backed up and verified on-chip, and the same version **with** the BSSID pin flashed over USB. It then came up on `192.168.178.66` within ~8 s of the reset and accepted shares. |
| `timsminer` (`192.168.178.61`, MAC ends `dd:3c`) | Flashed over USB. First boot of the **un-pinned** image: Wi-Fi associated, but DHCP timed out, the last-lease rung bound `.61`, the gateway probe failed (0/2 replies), DNS failed (error 202), no pool connection. After flashing the **pinned** variant: DHCP lease in 1.6 s, pool connected, shares accepted. |

Cause, established on `timsminer` from its boot log: without a pin the station
associated with the strongest mesh node (BSSID ending `…:d3:95`), which accepts the
association but relays nothing. The pinned node (BSSID ending `…:39:fc`, as on
`.80`) works. The v4.0.2-rc1 image itself boots and mines on BM1366. Blackhark
had run 25 days on a working node by chance; the reboot caused by the OTA drew a
new node. This is `PROVEN` for `timsminer` (log) and `PLAUSIBLE` for
`blackharkminer` (its OTA boot was not observed).

**Process fault, mine:** I sent the un-pinned blackhark image over OTA although
the earlier `timsminer` evidence (`dhcp ... 192.168.178.61`) and the repository
history (`829a87b`: "router never offers a lease") already showed this mesh
failure mode. The per-device variant for these units should have carried the
pin from the start; `tools/build_release.sh` takes it as an optional sixth
argument and defaults to empty, which is the trap.

Pools were changed on both units to the same wallet (the `blackharkminer`
owner's, prefix `dgb1qktg`); `timsminer` previously mined to another wallet. The
change was requested by the operator for this session and applied through
`PUT /api/system/pools/{0,1}`; worker names stay distinct. Blackhark's stored
fallback IP had pointed at `.61` (the other unit's address) and was corrected to
`.66`.

Final state, 2026-10-29 ~21:35: `blackharkminer` 339 GH/s, 1 accepted / 0
rejected after 43 s uptime; `timsminer` 270 GH/s, 77 accepted / 0 rejected after
682 s. `timsminer`'s hashrate is below the ~430 GH/s the board did in earlier
evidence; not investigated.

Backups (private): full-range USB reads of `0x0–0x410000` and `otadata` for both
units, verified against the chip (`esptool verify_flash`), SHA-256
`3860ab57…1d67d1bac` (`timsminer`) and `288ac0f7…875f35` (`blackharkminer`).
