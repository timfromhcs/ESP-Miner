# Changelog

All notable changes to this project are documented in this file.
This project adheres to [Semantic Versioning](https://semver.org/) and the **Truth Contract**.

---

## [4.0.1-stable] - 2026-10-18

Platform bring-up and AsicBoost hardening for the **Bitaxe Supra (Board 601,
BM1370)**, driven by the unit at `192.168.178.80`. Every item below was traced to
a specific line of shipped code and, where possible, to a measurement taken from
that unit before and after the change. The BM1366 devices (`.61`, `.66`) were
deliberately **not** reflashed; the shared-code items are marked as such.

The recurring theme is code that ran only on its error path. On a healthy miner
the error path is never taken, so the defect is invisible in normal operation and
shows up as a symptom with no obvious cause.

### Fixed

- **`notify_received`, `work_received` and the pool clock sync never ran (BM1366
  and BM1370).** All three were incremented only inside the
  coinbase-decode-failure branch of the `mining.notify` handler. A miner whose
  notifies decode cleanly — that is, a working miner — therefore reported
  `notifyReceived: 0`, `workReceived: 0`, `notifyDropped: 0` for the entire
  session, and never called `settimeofday()` at all. Confirmed on all three
  fleet members before the fix. The counters exist precisely to make
  `received + dropped == well-formed notifications` observable; a unit reporting
  three zeros looks indistinguishable from a pool that stopped sending jobs.
  Stratum V2 already did this correctly; V1 was the odd one out.
- **The BM1370 never re-programmed its ticket mask (P0).** Register `0x14` —
  the knob that sets how many nonces per second the chip pushes over UART — was
  written twice during `BM1370_init()`, both times with the static family default
  of 256, and never again. `ASIC_set_ticket_mask()` had no `case BM1370:` at all.
  On a ~1.5 TH/s chip a 256 mask asks for several million reports per second
  across a 1 Mbaud link; every one of them that arrives costs the ESP32 a full
  double-SHA256 in `test_nonce_value()` before being discarded. The BM1366 path
  was already correct. BM1370 now seeds the register from the pool difficulty at
  init and follows every `mining.set_difficulty`.
- **4.17 % UART baud mismatch on the BM1370 (P0).** `BM1370_set_max_baud()` wrote
  BT8D = 2, which clocks the chip at `25e6 / (3 * 8)` = 1 041 666.67 baud, but
  returned 1 000 000 to the host. The receiver sampled the wrong bit grid. This
  exact defect was found, documented and fixed on the BM1366 in v2.16.0 and never
  carried over; it matters more on the Supra, which produces roughly 2.5x the
  result rate over the same link. Both drivers now derive the host rate from the
  same expression they program into the chip, and `fast_uart` is plumbed through.
- **Per-nonce `ESP_LOGI` was drowning the log ring.** `asic_result_task` logged
  one INFO line per reported nonce. On a Supra that is thousands of lines per
  second, so the 128 KB ring buffer — and with it the entire boot trace and any
  real warning — was overwritten within seconds. A device in a bad state
  consequently looked clean in its own logs. Demoted to `ESP_LOGD`. This one
  change is what made the fault above diagnosable at all.
- **`version_bits` was recovered by XOR.** `rolled_version ^ job->version` only
  yields the rolled bits while the pool's version field happens to have none of
  them set. BIP9 signalling occupies bits [25:27], inside the rollable window, so
  a pool sending a signalling version made the XOR cancel exactly those bits and
  the share was submitted with a version the pool reconstructs differently — a
  rejection with no local symptom. Now masked with `ASIC_VERSION_ROLL_MASK`,
  which is correct in both cases.
- **The version-roll count was silently truncated.** `uint8_t hi = rolls >> 8`
  dropped the 17th bit. The firmware asks the pool for
  `"version-rolling.mask":"ffffffff"`, and a pool that grants it returns
  `0xFFFFFFFF` = 0x1FFFF rolls, which does not fit in the 16-bit register. Host
  and chip silently disagreed while the log reported 131071 rolls. Now clamped
  explicitly, with a warning when the clamp actually bites.
- **Chip configuration could wait a whole block cycle.** The
  `mining.set_difficulty` / `mining.set_version_mask` handlers sat inside the
  `if (new_work != NULL)` branch, so a change arriving while the queue was empty
  — or on an SV2 standard channel, where work is never re-dequeued at all — was
  not applied until the next `mining.notify`. On a pool that notifies every ~60 s
  that is the chip mining against a superseded ticket mask for a full block.
  Moved ahead of the blocking dequeue; the flags still stay pending until the
  ASIC is actually up.
- **Nonce space was sized from the requested frequency (BM1370).** The BM1366
  driver used `actual_frequency` with a fallback and a mismatch warning; the
  BM1370 used `frequency_value`. `ASIC_set_nonce_space()` additionally now
  refuses outright when the frequency is not positive, because the HCN register
  derivation divides by it and `NaN` → `uint32_t` is undefined — a failed PLL
  solve would otherwise write an arbitrary nonce-space size into register 0x10.
- **A version bump could be silently ignored.** `PROJECT_VER` is a CMake cache
  entry, so an existing build directory keeps whatever version it was first
  configured with and a bump in the source has no effect — which presents exactly
  like a failed OTA. Forced from the source default.
- **Three midstates were computed per job and never sent.** `construct_bm_job()`
  produced `midstate`/`midstate1`/`midstate2`/`midstate3` whenever a version mask
  was set, but only the BM1397 job frame can carry more than one; the other four
  drivers hardcode `num_midstates = 0x01`. That is three wasted SHA-256
  compressions and 96 bytes of dead payload per job, plus a `num_midstates = 4`
  claim that is wrong for four of the five ASIC families. Now gated on
  `ASIC_family_uses_rolled_midstates()`, with test coverage for both branches and
  a golden check that the rolled midstate matches an independent recomputation.
- **Two host test binaries were committed as build artefacts**, and the
  `simulation/` binaries generally were not ignored at all.
- **The CI unit-test project did not compile.** `components/connect/CMakeLists.txt`
  emitted `-DESP_MINER_CONFIG_AP_STUCK=${...}` unconditionally, so an unset CMake
  variable became `-DESP_MINER_CONFIG_AP_STUCK=` — an empty *string* to the
  preprocessor. `connect.c` compares that macro against `1` and `2`, so the test
  project, which defines none of the `ESP_MINER_*` variables, failed with
  `expected expression before '==' token`. The firmware build goes through the
  root `CMakeLists.txt`, which defaults the value to 2, and built fine — which is
  exactly why the break was invisible until the test project was built. A `-D` is
  now emitted only for a variable that has a value, the numeric knob defaults to
  `0` when unset, and all four macros gained `#ifndef` fallbacks in
  `nvs_config.h`. `ESP_MINER_STA_MAC` and `ESP_MINER_DHCP_HOSTNAME` previously had
  **no** header fallback at all and existed solely because of that unconditional
  `-D`.

### Added

- **Bitaxe Supra (Board 601 / BM1370) as a first-class build variant.**
  `tools/build_release.sh tim2` reproduces the unit's exact configuration —
  board version, family, ASIC, station MAC, AP pin, and the static fallback —
  so the image is reproducible instead of reconstructed from a CMake cache. The
  build now also prints the version it resolved, which is what makes a
  stale-cache mistake visible.
- **`tools/collect_telemetry.ps1`** — samples `/api/system/info` from one or more
  units into a CSV and derives the rates that actually matter (shares/min,
  rejects/min, J/TH) from the delta between first and last sample, so a before /
  after comparison does not depend on whatever the boot-time counters hold.

### Verified

- `192.168.178.80` (Bitaxe Supra, Board 601, BM1370, MAC `28:84:85:A6:1B:E0`)
  updated over **Wi-Fi OTA** (`POST /api/system/OTA`) from `v4.0.0-stable-tim2`
  to `v4.0.1-stable-tim2`, `factory` → `ota_0` → `ota_1`, address unchanged at
  `192.168.178.80`. Wi-Fi credentials, pools, frequencies, voltages and the
  static-fallback configuration were not touched.
  - Host CPU load **15.87 % → 14.10 %** and share submit latency **2.954 ms →
    2.495 ms** over matched 400 s captures — the ticket-mask fix removing wasted
    per-nonce verification work.
  - Effective share rate, difficulty-normalised: **+6.1 %**.
  - **0 shares rejected for any reason other than staleness.** Stale rejections
    run at **1.614/h**, against 1.581/h and 1.558/h on the two BM1366 units that
    have each been validated for 353 h. The Supra runs at 3.5× their hashrate and
    produces stale shares at the *same absolute rate*, which is what identifies the
    remainder as the inherent job-boundary race rather than a device defect.
  - AsicBoost confirmed on the wire: shares submitted with distinct rolled
    version-bit fields (`00040000`, `0007e000`, `000aa000`, `000e4000`, …), each
    answered `"result": true`.
  - Boot log retained in full; **zero `W`/`E` lines**.
- **The BM1366 units at `192.168.178.61` and `.66` were deliberately not
  reflashed** and served as the reference baseline for the measurements above.
  Both remain on their previous firmware with uptime past 353 h.

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
