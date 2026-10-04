# Plan: Bitaxe Ultra (BM1366) Optimization — v2.16.0

Status: **implemented**, pending hardware re-validation on `192.168.178.66`.
Baseline firmware: `v2.15.3-hardened`.
Evidence: [`evidence/192.168.178.66/`](../evidence/192.168.178.66/README.md)
(snapshot `20261004-003232`, ~41 min of ring-buffer log, 4 909 lines).

This is the design document referenced by
`components/stratum/include/notify_validate.h`,
`simulation/test_notify_validate.c`,
`main/http_server/axe-os/src/app/components/edit/edit.component.{ts,html}` and
`evidence/192.168.178.66/README.md`. It records *why* each change exists and
what was deliberately left undone.

---

## 1. Device under test

| Field | Value |
|---|---|
| IP | 192.168.178.66 — **reserved DHCP lease**, FRITZ!Box 7590 |
| Hostname / mDNS | `timsminer` / `timsminer.local` |
| MAC | `74:4D:BD:77:DD:3C` (efuse factory MAC) |
| Board | 201, 1× BM1366, 894 small cores, 4 hash domains |
| ASIC | BM1366, 485 MHz, 1200 mV nominal |
| Pool | `sha256.eu.mine.zpool.ca:3333` (SV1), fallback `…us…` |
| Coin | DigiByte SHA-256d |
| Baseline result | 433.6 TH/s expected, 88 425 shares accepted, **222 rejected (0.250 %)** |

---

## 2. Findings

### P0-1 — a job the firmware could not decode was mined anyway

`evidence/…/bitaxe-logs.clean.txt`:

```
E (2349263520) stratum_v1_task: Failed to process mining notification
I (2349264596) asic_result: ID: 72b7, ASIC nr: 0, Core: 89/2, … diff 347.7 of 720.
E (2349266712) asic_result: ID: 72b7, …
I (2349270625) asic_result: ID: 72b7, … diff 1497.4 of 720.
```

The decoder failed at `2349263520`, yet the job had **already been enqueued**, so
the chip burned a full ~6.7 s job interval on work that could never yield an
acceptable share. Three such occurrences in the capture window.

The frame itself:

| param | value | verdict |
|---|---|---|
| `[0]` job_id | `72b7` | ok |
| `[1]` prev_block | 64 hex chars | ok |
| `[2]` coinbase_1 | 112 hex chars (56 B) | ok hex, split mid-structure |
| `[3]` coinbase_2 | 138 hex chars (69 B) | ok hex |
| `[4]` merkle_branch | `[]` — **0 branches**, every other job had 5 | anomalous |
| `[5]` version | `00000006` — DGB algorithm bits 8..11 are 0 | anomalous |
| `[6]` nbits | `1900c185` — implies difficulty ~8.7e4, network is ~2.6e9 | anomalous |
| `[7]` ntime | `6ac179f3` | ok |

Every field is well-formed hex. The anomaly is **semantic**, which is precisely
why a hex validator cannot be the only gate.

### P0-2 — unchecked `hex2bin()` return left uninitialised merkle data on the chip

`parse_mining_notify()` `malloc()`ed the merkle array and then filled it from
the pool without checking that `hex2bin()` actually converted a full 32 bytes.
A short branch string therefore left part of a freshly allocated buffer
uninitialised, and the chip hashed it. Fixed by validating first and checking
the conversion result.

### P1-1 — 0.250 % of shares rejected as "Invalid job id" (stratum error 21)

Four pool rejections in the window. This is the unavoidable race between a new
job arriving and a share still in flight — not a client defect — but it was
invisible, lumped into one `sharesRejected` number with no way to tell a latency
race from a real bug.

### P1-2 — domain imbalance of 36 %

`131.0 / 100.9 / 96.6 / 113.8` TH/s across the four hash domains.

### P1-3 — Vcore flapping 1188–1239 mV around a 1200 mV set point

### P2-1 — 3× `httpd_sock_err: error in recv : 104` (ECONNRESET, web/websocket clients)

### P2-2 — PSRAM healthy

7 558 508 B free, `minFreeHeap` unchanged over the window → no leak. Recorded so
future work does not re-investigate it.

---

## 3. Changes

### 3.1 Stratum notify validation (P0-1, P0-2)

New dependency-free module `components/stratum/notify_validate.{c,h}`, unit
tested on the host **and** under QEMU Unity:

* `stratum_v1_hex_is_valid()` — exact-length all-hex.
* `stratum_v1_nbits_is_plausible()` — rejects zero mantissa, exponent outside
  `0x03..0x30`, and the negative-mantissa bit.
* `stratum_v1_notify_validate()` — job id, prev-block hash, both coinbase
  halves, every merkle branch, version/nbits/ntime.

Wiring:

* `stratum_api.c` type-checks `params[0..3]` before dereferencing (they were
  previously unchecked — a truncated notify could crash the parser), validates
  before allocating, and now **checks `hex2bin()`**.
* `stratum_v1_task.c` moved the coinbase decode **before** `queue_enqueue()`.
  `decode_mining_notification()` returns `bool`; on failure the job is freed,
  counted, and the ASIC keeps its current work. This is the actual fix for P0-1.

Division of responsibility, pinned by
`simulation/test_notify_validate.c::main()`: job `72b7` **passes** field
validation (its hex is well-formed) and is caught by the decoder instead. The
hex validator guards memory safety; the decoder guards correctness.

### 3.2 Observability (P1-1)

New API fields, plumbed through `openapi.yaml` → generated client → AxeOS:

| field | meaning |
|---|---|
| `notifyReceived` | notifies turned into real chip work |
| `notifyDropped` | notifies refused (parse-level **and** decode-level) |
| `sharesRejectedStale` | stratum error 21 — the latency race |
| `sharesRejectedOther` | everything else — a real client-side defect |

`notifyDropped` counts **both** rejection paths. `STRATUM_V1_parse()` sets
`->method` before dispatching, so the caller can distinguish a refused
`mining.notify` from any other parse failure; without that the documented
`received + dropped = total` invariant would not hold.

### 3.3 ASIC link correctness

* **UART baud mismatch.** The host derived BT8D for an exact 1 041 666 baud but
  programmed 1 000 000 — a **4.17 % mismatch** against a chip actually clocked
  at 25 MHz / 24. `BM1366_set_max_baud()` now returns the exact host rate.
* **Ticket mask.** Now tracks `mining.set_difficulty`
  (`ASIC_set_ticket_mask()`), capped at `ASIC_TICKET_MASK_MAX_DIFFICULTY`.
  Note the mask is the largest power of two **≤** difficulty, so at pool
  difficulty 720 it is 512 — looser than the pool, not tighter.

### 3.4 Thermal policy — ATM

New `main/thermal/atm_policy.{c,h}`, dependency-free and host-tested. The old
two-state latch had three concrete defects:

1. `1000 mV − 100 mV = 900 mV` is below the TPS546/TPS40305 floor on some
   boards → bogus "Power Fault" with a healthy PSU.
2. The reduction was written to NVS, so a transient event permanently cost
   hashrate.
3. No anti-hunting memory → oscillation around the threshold.

The module fixes all three: regulator-floor clamp, hysteresis buffer, frequency
and voltage moving together, a `min_hold_s` dwell, and Braiins-style "DPS
memory" (extra margin required to undo a downshift).

**In this revision, additionally:**

* **Transient derates are no longer written to NVS.** NVS stays the single
  source of truth for the user's setting; the derate lives in `atm_state` and is
  applied through an explicit override in the apply path. Previously a reboot
  during a derate re-read the derated value as the new *ceiling*, which is how
  defect 2 survived in practice.
* **The shutdown latch is now clearable** (`atm_clear_shutdown()`), and the hard
  backstop's recovery path calls it after a real cooldown. Previously
  `atm_init()` ran once before the loop, so a single ≥75 °C reading disabled
  thermal management for the rest of the task's life.
* **The ceiling follows the user's settings** (`atm_set_baseline()`). It used to
  be captured once at task start, so lowering the target in AxeOS would be
  silently fought by the old, higher ceiling.
* **Fan-relief path implemented.** The design comment promised that a
  meaningfully better fan duty cycle could substitute for the extra temperature
  margin; it was never written. It now is (`fan_relief_percent`, 20 points).
* Threshold ladder is now three distinct tiers and the misleading comment
  ("`hot_temp_c` sits above `THROTTLE_TEMP`" — it is 68 vs 75, i.e. *below*) is
  corrected. The hard VR cutoff stays owned solely by the legacy backstop.

### 3.5 V/F autotuner

New `main/power/vf_tuner.{c,h}`: holds frequency, minimises voltage, gated on
≥95 % of expected hashrate and ≤0.1 % errors, reverting to the last proven
voltage on failure. Opt-in via `autotuneVoltage` **because it does** persist to
NVS (a proven-stable operating point is worth keeping).

`VF_ACTION_ABORT_THERMAL` was declared and named but could never be returned —
the guard was `chip_temp_c >= 0.0 && !acceptance_passed()`, where the left
operand is true for any sane reading. It is now a real thermal abort above the
board's 65 °C continuous limit, and the caller disables autotuning on it.

### 3.6 Network / WiFi — corrected

This is the part that needed research rather than guessing; see
[`NETWORKING.md`](NETWORKING.md).

The previous revision tried to defeat a DHCP lease conflict by **overriding the
STA MAC** to `74:4D:BD:77:DD:3D` and forcing the DHCP hostname to
`bitaxe-dd3c`. That was wrong in four independent ways:

1. **It defeats the reservation.** `192.168.178.66` is a router-side DHCP
   reservation keyed on the real MAC. Changing the MAC means a different lease —
   and the static-IP fallback that used to paper over this had just been deleted
   in the same change, leaving DHCP as the only path to an address.
2. **It was called at the wrong point in the lifecycle.** `esp_wifi_set_mac()`
   must be called *after* `esp_wifi_start()`; it was called before, and the
   return value was discarded — which is why `…DD:3C` still appears in the
   device's own API output.
3. **It was global, not per-device.** Every unit of every board type on every
   network would claim the same MAC.
4. **Hostname and MAC contradicted each other** (`bitaxe-dd3c` vs `…DD:3D`).

The correct mechanism already exists in ESP-IDF and is enabled in `sdkconfig`:

```
CONFIG_LWIP_DHCP_DOES_ARP_CHECK=y
```

> *Simple ARP check, default option: sends two ARP probes and only declines the
> offer if a reply for the offered IP comes from a **different MAC address than
> the interface MAC**. This is fast (about 1–2 seconds) and avoids false
> conflicts on networks where the AP echoes the client's MAC in ARP replies.*

Critically, ESP-IDF's docs warn **against** the obvious-looking alternative:

> *Address Conflict Detection (ACD) … Some access points respond to ARP probes
> with the client's own MAC for the offered IP; upstream behavior treats any
> matching sender IP during PROBING as a conflict, **which can cause repeated
> DHCP DECLINEs on such networks**.*

An FRITZ!Box that echoes the client MAC in ARP replies is exactly that case, so
ACD would have made the original symptom *worse*. The project stays on the ARP
check.

Also corrected in the same area:

* **No more wiping a valid lease.** `esp_netif_set_ip_info()` was called with a
  zeroed struct on *every* association; ESP-IDF requires the DHCP client to be
  stopped first, and it forced a needless renegotiation on every roam, dropping
  the stratum socket and the web UI.
* **No more `stop`→`start` DHCP bounce**, which resets the DHCP xid.
* **No more permanent `dhcp` DEBUG logging.** It was raised to DEBUG twice and
  never reverted, permanently inflating the 512 KB log ring buffer.
* **The DHCP hostname comes from NVS** (`timsminer`), not a hardcoded literal.
* `CONFIG_LWIP_DHCP_RESTORE_LAST_IP` was considered and deliberately left
  **off**: restoring a cached lease across a power cut bypasses the ARP
  conflict check, which is the wrong trade for a device whose address is already
  reserved.

---

## 4. Deliberately **not** done

| Item | Why |
|---|---|
| Stricter `nBits` plausibility | `1900c185` (difficulty ~8.7e4 vs network ~2.6e9) is a real anomaly the current heuristic lets through. Tightening it without hardware validation risks dropping **valid** jobs, which costs real hashrate. Deferred to Phase 2; `notifyDropped` makes it observable first. |
| Domain-imbalance (P1-2) remediation | The 36 % spread needs a per-domain job-rotation investigation with hardware correlation; no code change is justified from a single snapshot. |
| Vcore flapping (P1-3) | 1188–1239 mV around a 1200 mV set point is within regulator tolerance for this board. Needs a longer capture before acting. |
| `CONFIG_LWIP_DHCP_RESTORE_LAST_IP` | Bypasses the conflict check; see above. |
| Disabling IPv6 | Would save ~39 KB flash / ~7 KB RAM, but the device currently holds a valid IPv6 link-local and the win is not worth the regression risk in this release. |

---

## 5. Verification

| Layer | How | Result |
|---|---|---|
| Host — notify validator | `simulation/test_notify_validate.c` | 33 checks |
| Host — ATM + V/F | `simulation/test_power_policy.c` | 77 checks |
| Firmware — Unity/QEMU | `components/stratum/test/test_notify_validate.c` via `idf.py build test` | CI |
| Frontend | `npm run test:ci` | 60 specs |
| Hardware | OTA to `192.168.178.66`, NVS preserved | see `evidence/192.168.178.66/` |

The host suites are dependency-free C11 and run under
`-Wall -Wextra -Werror -O2`; they execute in the `host-tests` CI job in seconds,
without waiting for the QEMU job.

**Known local quirk:** on Windows, `npm run test:ci` prints `TOTAL: 60 SUCCESS`
but exits `1` because Karma's browser teardown raises an uncaught
`ECONNRESET`. This is a Windows-only artifact — CI on Linux is green — so the
Karma config is intentionally left alone rather than papering over it with a
disconnect tolerance that could mask a genuine browser crash.