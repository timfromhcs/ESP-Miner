# Changelog

All notable changes to this project are documented in this file.
This project adheres to [Semantic Versioning](https://semver.org/) and the **Truth Contract**.

---

## [2.16.0-hardened] - 2026-10-04

Driven by a 41-minute ring-buffer capture from the production Bitaxe Ultra at
`192.168.178.66`. Full rationale, findings and the deliberate non-goals:
[`docs/PLAN_BITAXE_BM1366_OPTIMIZATION.md`](docs/PLAN_BITAXE_BM1366_OPTIMIZATION.md).

### Fixed
- **Undecodable jobs were mined anyway (P0).** `mining.notify` frames whose
  coinbase could not be decoded were still enqueued, so the chip burned a full
  ~6.7 s job interval on work that could never produce an acceptable share. The
  decode now gates the enqueue; the job is dropped and the ASIC keeps its
  current work.
- **Unchecked `hex2bin()` on merkle branches (P0).** A short branch string left
  part of a freshly `malloc()`ed buffer uninitialised, and the chip hashed it.
  Validation now runs before allocation and the conversion result is checked.
- **Unchecked `params[]` dereferences.** `params[0..3]` were dereferenced without
  a type check; a truncated or hostile notify could crash the parser.
- **STA MAC was overridden in firmware.** A hardcoded `74:4D:BD:77:DD:3D`
  (plus a hardcoded `bitaxe-dd3c` DHCP hostname) was introduced to dodge a lease
  conflict. That defeats the router-side reservation for `192.168.178.66`, was
  global rather than per-device, contradicted itself, was called before
  `esp_wifi_start()` against the documented contract, and had its return value
  discarded. Removed — lease conflicts are handled by the DHCP client itself via
  `CONFIG_LWIP_DHCP_DOES_ARP_CHECK`.
- **A valid DHCP lease was wiped on every re-association.** `esp_netif_set_ip_info()`
  was called with a zeroed struct on each association (ESP-IDF requires the DHCP
  client to be stopped first), forcing needless renegotiation on every roam and
  dropping the stratum socket and web UI.
- **DHCP client was bounced `stop`→`start`**, resetting the DHCP xid.
- **`dhcp` log level was pinned to DEBUG permanently**, inflating the 512 KB
  ring buffer. Now raised only while acquiring a lease and restored after.
- **UART baud mismatch.** The host derived BT8D for an exact 1 041 666 baud but
  programmed 1 000 000 — a 4.17 % mismatch against a chip clocked at 25 MHz/24.
- **`ASICModel` lost its `required` marker** in `openapi.yaml`, silently making
  the generated Angular field optional.
- **ATM shutdown latch could never clear.** `atm_init()` ran once before the
  task loop, so one ≥75 °C reading disabled thermal management for the rest of
  the task's life. The hard backstop's recovery path now clears it.
- **Transient ATM derates were persisted to NVS**, so a reboot during a derate
  re-read the derated value as the new ceiling. Derates are now in-memory only;
  NVS remains the single source of truth for the user's setting.
- **ATM ceiling was captured once at task start**, so lowering the target in
  AxeOS was silently fought by the old, higher value. It now follows the setting.
- **`VF_ACTION_ABORT_THERMAL` was unreachable.** The guard was
  `chip_temp_c >= 0.0 && !acceptance_passed()`, where the left operand is true
  for any sane reading. Now a real abort above the 65 °C continuous limit.
- **`notifyDropped` undercounted.** Parse-level rejections were logged but never
  counted, breaking the documented `received + dropped` invariant.
- **ATM/VR threshold comment was inverted** and the VR emergency cutoff was
  duplicated with the legacy backstop at the same value.

### Removed
- **The hardcoded static-IP fallback.** The firmware-level constant is gone: it was
  baked into every unit on every network and claimed an address it could not prove
  free. See "Added" for the per-device replacement.

### Added
- **Per-device static IPv4 fallback** (`useStaticFallback`, `staticIp`,
  `staticGateway`, `staticSubnet`, `staticDns`). **Defaults to off with no
  address baked in** - a default that names a specific address would make every
  unit built from this firmware claim an address it was never given, which on a
  network where that address is reserved is exactly the duplicate-IP conflict the
  feature exists to survive. Enable it per device, for an address reserved on that
  device's router, via AxeOS or `PATCH /api/system`. Consulted only after DHCP
  has genuinely failed and the retry/recovery cycles are exhausted. Supersedes the
  removed hardcoded fallback.
- **components/stratum/notify_validate.{h,c}** — dependency-free `mining.notify`
  field validator (job id, prev-block, both coinbase halves, every merkle branch,
  version/nbits/ntime) with an `nBits` plausibility heuristic. Tested on the host
  and under QEMU Unity.
- **main/thermal/atm_policy.{h,c}** — proactive thermal management: hysteresis
  buffer, frequency and voltage moving together, regulator-floor clamp,
  anti-hunting "DPS memory", minimum dwell, and a fan-relief path that lets a
  materially better duty cycle substitute for the extra temperature margin.
- **main/power/vf_tuner.{h,c}** — opt-in closed-loop voltage minimiser that
  holds frequency and reverts to the last proven-stable voltage on failure.
- **API/UI:** `notifyReceived`, `notifyDropped`, `sharesRejectedStale`,
  `sharesRejectedOther`, `asicFastUart`, `autotuneVoltage`. The home view now
  separates the unavoidable stale-share race from real defects.
- **`host-tests` CI job** running four dependency-free host binaries in seconds,
  without waiting for the QEMU job.
- **DHCP client diagnostics** — `esp_netif_dhcpc_start()`'s return value, the
  live client state and link state on every retry. This is what distinguishes
  "server never answers" from "client never started", which look identical from
  outside the device.
- **docs/PLAN_BITAXE_BM1366_OPTIMIZATION.md** — the design and rationale
  document, plus `evidence/192.168.178.66/` device captures.

### Fixed (found on hardware, v2.16.0)
- **The DHCP client was never started on a cold boot.** Narrowing the start
  condition to `ESP_NETIF_DHCP_STOPPED` dropped `ESP_NETIF_DHCP_INIT` — the state
  a freshly booted client sits in — so no lease was ever requested.
- **The static fallback bound an address that never reached LwIP.**
  `esp_netif_set_ip_info()` only pushes into the TCP/IP stack *"if the interface
  is up"*, and `esp_netif_dhcpc_stop()` leaves the netif administratively down, so
  the address landed only in esp-netif's copy. LwIP kept `ip_addr == 0` and never
  answered ARP — the same "unreachable from its own subnet, no ARP entry" symptom
  the original hardcoded fallback had. Order is now
  `dhcpc_stop → netif_set_up → set_ip_info`, and success is asserted with
  `esp_netif_get_ip_info()` plus `esp_netif_is_netif_up()`.

### Hardware validation (`192.168.178.66`)
| Metric | v2.15.3 | v2.16.0 |
|---|---|---|
| DHCP lease | not obtained; static fallback used | **2 966 ms, real lease** |
| Hashrate (1 h avg) | 433.0 TH/s | 429.7 – 430.2 TH/s |
| Shares rejected | 222 / 88 425 (0.250 %) | 1 / 80, and `stale=1, other=0` |
| Vcore | 1188 – 1239 mV flapping | 1192 mV |
| Free PSRAM | 7 558 508 B | 7 571 680 B |

NVS was preserved across every flash (app-only OTA plus USB `esptool` writes);
pool addresses, Wi-Fi credentials, wallet and hostname were untouched. The UART
fix is visible on-device as
`Setting chip UART BT8D=2 -> 1041666 baud (host programmed to match)`.

### Known limitations
- Stricter `nBits` plausibility is deferred: `1900c185` implies difficulty ~8.7e4
  against a ~2.6e9 network and is still accepted. Tightening it without hardware
  validation risks dropping valid jobs.
- Domain imbalance (36 % spread) and Vcore flapping (1188–1239 mV around a
  1200 mV set point) are recorded but not yet acted on; both need longer
  captures before any code change is justified. Vcore flapping is improved
  (1192 mV steady in the post-flash capture); the domain spread is not.
- On Windows, `npm run test:ci` prints `TOTAL: n SUCCESS` but exits `1` because
  Karma's browser teardown raises an uncaught `ECONNRESET`. Windows-only; CI on
  Linux is green, so the Karma config is intentionally left unchanged.

---

## [2.15.3-hardened] - 2026-09-06

### Changed
- **Wi-Fi Fast Scan:** Enabled `WIFI_FAST_SCAN` for known STA connections, significantly reducing scan overhead and channel dwell time during startup.
- **Selective SoftAP Initialization:** SoftAP is initialized strictly when no SSID is configured in NVS, preventing dual STA+AP coexistence radio contention on boot.
- **Exponential/Staged Wi-Fi Reconnect Backoff:** Replaced fixed 5s disconnect wait with staged 500ms -> 2000ms -> 5000ms retry intervals for rapid recovery from brief router drops.
- **BLE / Wi-Fi Coexistence Hardening:** Increased BLE advertising grace period on boot to 15s when an SSID exists, preventing NimBLE 2.4 GHz radio packet collision during critical DHCP handshake (e.g. on FRITZ!Box routers).
- **Stratum Reconnect Loop:** Reduced Wi-Fi link check delay in Stratum V1/V2 worker tasks from 10s to 1s, achieving near-instantaneous reconnection when the network recovers.

### Hardware Validation
- **Device A (192.168.178.66 / USB COM3):**
  - IP acquisition on FRITZ!Box: Instantaneous without radio contention (1,654 ms).
  - Mining: Sustained 435.81 GH/s mean @ 485 MHz / 1.206 V VCore (12.40 W, 28.45 J/TH).
  - Stratum: 66 shares accepted, 0 rejects (0.00%), 0 duplicate nonces.
- **Device B (192.168.178.61 / Network OTA):**
  - Upgraded via safe OTA from v2.14.0 to v2.15.3-hardened.
  - Complete pre-upgrade NVS backup and 100% parameter restoration verified.
  - Mining: Sustained 434.22 GH/s mean @ 485 MHz / 1.200 V (12.31 W, 28.35 J/TH).
  - Stratum: 37 shares accepted, 0 rejects (0.00%), completely eliminated 253 "Invalid job id" rejections from v2.14.0.
  - Recovery: Pause / Resume verified without reboot or stale work.

---

## [2.15.2-hardened] - 2026-09-05


### Added
- Architectural and Inspection documentation (`docs/ARCHITECTURE.md`, `docs/BUILD.md`, `docs/FLASHING.md`).
- Hardware-in-the-Loop validation evidence and test matrix (`docs/HARDWARE_VALIDATION.md`, `docs/EVIDENCE.md`).
- Release provenance and checksum manifest (`docs/RELEASE.md`, `evidence/hashes/manifest.json`).

### Changed
- Refactored Wi-Fi STA DHCP client startup sequence to trigger exclusively upon `WIFI_EVENT_STA_CONNECTED`.
- Replaced destructive `esp_wifi_disconnect()` calls on DHCP lease timeouts with non-destructive ``esp_netif_dhcpc_stop() ` + `esp_netif_dhcpc_start()` discovery renewals.

### Fixed
- Unnecessary Wi-Fi link flapping in heavily congested 2.4 GHz environments.

### Hardware Validation
- Validated via physical USB Serial (COM3) on Bitaxe Ultra (Board 201, BM1366 ASIC, 112 Cores).
- Measured: 485 MHz @ 1200 mV, 44.9 C - 46.4 C, live Stratum shares accepted by solopool.eu, Overt ASICBoost version-rolling register 0xA4 programming verified.

### Known Limitations
- ASIC-side midstate reuse remains NOT PROVEN due to OPAQUE silicon internals.
- BM1366 nonce space traversal cannot be externally partitioned beyond HCN calculation.
