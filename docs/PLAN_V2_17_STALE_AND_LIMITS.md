# Plan: v2.17.0 — stale shares, faster recovery ladder, honest limits

Status: **proposed, not implemented. Nothing in this plan has been pushed.**
Author basis: 2026-10-04 session, device logs from `192.168.178.66` and
`192.168.178.61`, plus external research (all citations in §7).
Supersedes nothing; extends [`PLAN_BITAXE_BM1366_OPTIMIZATION.md`](PLAN_BITAXE_BM1366_OPTIMIZATION.md).

---

## 0. The finding that matters

`192.168.178.66` currently reports:

```
shares 384 / 59      stale 59      other 0      notifyReceived 188   notifyDropped 18
hashrate_1h 431.2 TH/s
```

**13 % stale rejection.** In the earlier v2.16.0 capture the same firmware measured
**1 rejection in 80 shares**. So this is not a property of the code I shipped — it
is a new, findable condition. Everything else in this plan is secondary.

Log correlation (`t≈3040688…3041093`):

```
tx id=436 job d152   ->  rx [21,"Invalid job id"]
tx id=437 job d152   ->  rx [21,"Invalid job id"]
tx id=438 job d152   ->  rx [21,"Invalid job id"]
```

Consecutive submissions **under one already-dead `job_id`**, interleaved with
accepted shares. That is the signature of a client that is knowingly continuing
to hash a job the pool has retired.

---

## 1. P0 — stop stranding the ASIC on a dead job

### 1.1 Root cause (verified from the tree, arithmetically consistent)

`main/tasks/stratum_v1_task.c` drops an undecodable `mining.notify` and keeps the
ASIC on the previous job:

```c
} else if (!decode_mining_notification(GLOBAL_STATE, …)) {
    GLOBAL_STATE->notify_dropped++;
    // Never enqueue work we could not decode. Mining it would
    // burn the full job interval producing guaranteed-stale shares.
```

The comment's premise is wrong. The cost is not one job interval. Zpool issues a
new `job_id` roughly **every 31 s** (measured across four independent prevhash
groups in the baseline capture: 31.0 / 31.1 / 31.3 / 31.4 s). So refusing a job
and staying on the old one strands the chip for up to **31 s**, not 2 s — and
`create_jobs_task` still holds the previous `job_id` in `current_work` and
`active_jobs[]`. Nothing invalidates it on a drop.

Two compounding gaps:

* the unstick path needs the *next* successfully-decoded notify;
* if that notify arrives with `clean_jobs=false`, `create_jobs_task` takes
  `continue` **without programming the ASIC** — so the strand persists even
  though a newer valid job is in hand. (This hazard also exists on upstream
  `master`.)

Arithmetic reconciliation (share interval = `2^32·D/H`, `H` = 433.6 TH/s):

| case | notifies dropped | share interval | expected rejects | observed |
|---|---|---|---|---|
| baseline (D≈720) | 0 | 7.13 s | ~10 tail-race | **4** |
| current (D≈416) | 18 / 206 | 4.12 s | ~22 tail-race | **59** |

Excess ≈ 37, well inside the stranding envelope (18 drops × 31 s ÷ 4.12 s ≈ 135
capacity). **Inference, not measurement** — §1.2 is designed to settle it.

### 1.2 Fix, in three parts

**(a) Log why a notify was dropped.** `stratum_v1_task.c` calls
`decode_mining_notification()` and **discards the reason string**, while
`notify_validate.c` already produces precise ones (`"coinbase length is odd"`,
`"coinbase contains non-hex characters"`, `"coinbase N bytes exceeds limit"`,
implausible-nbits). Without this we cannot tell whether 18/206 drops is a Zpool
protocol quirk or our own validator being wrong. **Highest information value per
line in this whole plan. Do it first, ship it, then decide.**

**(b) `force_clean_pending` flag.** Set it in the drop branch. In
`create_jobs_task`, when set, ignore `clean_jobs` and program the ASIC from the
next dequeued work item. This closes both the drop window and the
`!clean → continue` strand.

**(c) Only then, if drops prove frequent, consider asking the pool for fresh
work** rather than idling. Not before (a) tells us why.

### 1.3 Latency and difficulty (independent levers, cheap)

* **`TCP_NODELAY` on the stratum socket.** Upstream merged this in v2.14.0b4
  ([#1722]) explicitly "to fix SV2 submit latency". Nagle can add up to 40 ms to
  every `mining.submit`. **Verify our fork sets it; if not, it is a one-line
  fix** and pure win.
* **Pin pool difficulty higher.** Reject rate ≈ exposure / share-interval, and
  share-interval = `2^32·D/H`. `stratumSuggestedDifficulty` is currently `1000`,
  yet observed difficulty walked `720 → 416 → 256`. Each downward varDiff step
  raises the reject rate *proportionally*. Aim for one share per 10–30 s.
  **Zero firmware risk — config only.** Caveat: the pool's varDiff may override;
  measure the resulting difficulty before concluding.
* **Do NOT shorten the ASIC job interval.** `default_asic_timeout` = 2000 ms is
  a **host-side send cadence only** — there is no job-timeout register in the
  BM1366 driver. Re-sending the *same* job more often cannot make it fresher; it
  only churns the chip's 16 job slots (slot reuse every 16 sends) and adds UART
  traffic. Upstream [#248] argues the opposite direction: version-rolling
  silicon like BM1366 *"should be able to have much longer job intervals, on the
  order of 10s of seconds."* Investigate lengthening; do not shorten.

### 1.4 Cherry-picks worth taking from upstream

Our fork is ~9 days behind `bitaxeorg/ESP-Miner` master and diverged substantially.

| item | state | why |
|---|---|---|
| [#1926] clamp job interval at zero | merged 2026-08-29 | `timeout_ms` is decremented but **not reset** on the `!clean → continue` path, can go negative, `pthread_cond_timedwait` returns `ETIMEDOUT` immediately → **the interval stops being rate-limited** → job churn. Directly on the stale path. |
| [#1855] source-epoch tagging on queued work | merged | makes "is this work still current?" explicit and testable instead of emergent. Pairs with §1.2(b). |
| [#1989] reject invalid PLL settings, bound thermal recovery | **open** | `pll_get_parameters()` has **no error return** and can yield all-zero dividers ([#1864] fixed the NVS-side guard). Also: *nonce-space timing must use the **applied** frequency, not the requested one* — see §1.5. |
| [#1722] `TCP_NODELAY` | merged | §1.3. |
| [#1806] flatline-of-death watchdog on **pool-accepted** shares | open | timeout = 25× expected share interval at current difficulty, floor 10 min; false-trigger ≈ `e^-25`. Garbage nonces can fool local counters but cannot pass pool validation. Cheap insurance against the "Flatline of Death" ([#1053]). |
| [#1955] dynamic ticket mask | open | upstream: a fixed diff-256 mask yields ~8 shares/s, *"pressure on the ESP32 as each share needs to be verified, on the logging and on the serial bus."* Directly cuts share-path latency. |

### 1.5 One concrete suspicion in our own code

`components/asic/bm1366.c:305` computes nonce space from the **requested**
frequency:

```c
float frequency = GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value;
BM1366_set_nonce_space(1.0, frequency, asic_count, cores);
```

HCN (`hcn_max = hcn_space · FREQ_MULT / frequency · 0.5`) is the **nonce limiter** —
how much of its slice the chip burns before needing a new job. If HCN is sized for
485 MHz but the chip is actually clocked lower (or the PLL never locked), the
chip's nonce slice is wrong-sized relative to the host's cadence: too large and
late nonces all belong to a dead job. Upstream #1989 lists exactly this as a bug.
**Decisive experiment: log requested vs applied frequency at that line, plus
measured job interval and stale counts.** Cheap, and it either confirms or kills
the hypothesis.

### 1.6 Verification for P0

1. Add counters: `notifyDroppedByReason[reason]`, `stranded_ms_total`,
   `sharesRejectedStale`.
2. Run ≥4 h on one device at fixed difficulty. **Target: stale < 2 %** (baseline
   run was 1/80 = 1.25 %). Compare against the same-duration pre-change run.
3. Confirm the tail-race bound holds: rejects should track
   `notify_latency / share_interval`, i.e. ~0.2 s / 7.1 s at D=720.
4. Reject the stranding theory if rejections appear with **no nearby drop** and
   job age ≈31 s — in that case the cause is elsewhere and we re-plan.

---

## 2. P1 — make the recovery ladder fast (explicitly requested: "die Leiter zu langsam")

Current cost before any fallback fires: 5 retries with backoff
≈ 5.2 + 6.4 + 7.6 + 8.8 + 10.0 ≈ **38 s**, then 3 recovery cycles of 5–7 s each
≈ **18 s**, then `esp_wifi_disconnect()` and a full re-association. **≈1 minute of
dead time minimum**, repeatedly, before the ladder is even consulted.

Proposal — **phase the ladder by confidence instead of by wall-clock**:

| phase | trigger | action | budget |
|---|---|---|---|
| P0 | `STA_CONNECTED` | DHCP as now | unchanged |
| P1 | 1st retry timeout (~8 s) | consult ladder: **last real lease** | 0 s — address already in hand |
| P2 | retries exhausted | re-associate Wi-Fi, then ladder again | ~2 s |
| P3 | 2nd exhaustion | `esp_wifi_disconnect()` + ladder | ~2 s |
| P4 | 3rd exhaustion | longer backoff, keep trying forever | as now |

Rationale: rung 1 (the last real DHCP lease) is **free** — no negotiation, no
timeout, and it is an address this unit is already known to hold, so it cannot
collide with a stranger. There is no reason to make the operator opt in for it,
and no reason to make the miner idling for a minute before trying it.

Changes:
* consult the ladder at the **first** retry timeout, not only after all 5;
* drop `DHCP_RETRY_MAX` from 5 → **3** with tighter backoff (3 s / 4 s / 5 s);
* keep DHCP running in the background so a real lease still wins the moment it
  arrives — a fallback must not permanently disable DHCP;
* **when to re-arm DHCP:** today a successful fallback returns and never retries
  DHCP again. Instead, once online via a fallback, keep a slow DHCP probe
  (e.g. every 5 min) and migrate to the real lease if one appears.

Safety: rung 1 only ever replays an address this device already legitimately
held; rung 2 is operator-configured and requires an explicit
`useStaticFallback`. Neither can invent an address.

---

## 3. Memory — the honest answer is "you don't have a problem"

Measured on device: **7 571 680 B PSRAM free (93 %)**, 86 KB of 288 KB internal
SRAM free (30 %), `minFreeHeap` flat over the capture window. The plan document
already closed this as no-leak. So most "use more SRAM" ideas solve a problem we
do not have, and this section is deliberately short.

Two cheap, low-risk wins:

| change | gain | risk |
|---|---|---|
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 8192 → 3584 | ~4 KB internal SRAM, permanent | near-zero: `app_main()` returns quickly and the task then only parks |
| `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 16384 → 32768, then 65536 | moves lwIP pbuf pools / TLS / cJSON trees out of internal SRAM | medium — anything genuinely needing internal DRAM under the threshold silently lands in PSRAM; measure internal free heap at each step |

Explicitly **not** doing:
* `CONFIG_FREERTOS_PLACE_TASK_STACKS_IN_EXT_RAM` globally — the docs are clear
  that flash/NVS/OTA paths run with the CPU cache disabled and *must* have an
  internal-RAM stack. This firmware writes NVS constantly and does OTA. Use
  `esp_flash_dispatcher` or a dedicated internal-stack task first, or not at all.
* PSRAM 80 → 120 MHz — documented as supported for octal on S3, but the MEMSPI
  clock is **shared with flash**, so it is a coupled experiment, not a free win.
* `XIP_FROM_PSRAM` / `FETCH_INSTRUCTIONS` / `RODATA` — trades the whole
  PSRAM-vs-flash cache profile for flash savings we do not need (16 MB flash,
  AxeOS already gzipped into the image).

---

## 4. ASIC Boost — do not chase it

* **AsicBoost is a 2016 silicon technique**, not firmware: generate many header
  candidates sharing Message Schedule 1 and hoist the nonce loop. Paper claims
  ~20 % gate-count reduction [arXiv:1604.00575].
* **Modern ASICs already implement it.** Overt AsicBoost (BIP310/BIP320 version
  bits) is negotiated in `mining.configure` and is in every current-gen chip.
  Covert AsicBoost has been effectively dead since SegWit (2017).
* ⇒ **There is no firmware switch and no firmware hashrate gain available.** Any
  pool selling "AsicBoost as a premium feature" is reselling a standard
  handshake option.
* ⚠️ **"LuxOS Firmware Reject" does not exist.** Zero hits across Luxor's full
  documentation index, feature pages, API reference and changelog. Do not act on
  it. "ASICPRO" could not be identified either.
* ⚠️ **LuxOS, Vnish and Braiins do not support Bitaxe at all.** Their headline
  features — per-chip tuning, chain balancing, hashboard voting, per-hashboard
  disable, immersion mode — are structurally impossible on **one** BM1366, one
  voltage rail, one fan. Vnish's verified 1.8–2.8 % dev fee on a 430 TH/s device
  would be a net loss against a 0.5 % pool fee.

**The real hashrate levers, and where we already stand:**

| lever | status |
|---|---|
| nonce-space coverage | ✅ already 100 % (`BM1366_set_nonce_space(1.0, …)`, via merged upstream #420) |
| job-interval rate limiting | ❌ missing [#1926] |
| not discarding work on malformed jobs | ✅ fixed in v2.16.0 |
| not wedging the result path | ❌ open upstream [#1053], [#1806], [#1820] |
| share-path latency | ❌ verify `TCP_NODELAY` [#1722] |
| PLL solution choice | 🔶 `pll_get_parameters()` *searches* rather than table-lookup, so among the 2424 achievable frequencies with **two disjoint VCO bands** (none between 3000–4000 MHz) it may pick a valid but power-hungry solution. Efficiency lever, not hashrate. [#1044], [#1864] |
| CRC5 | ✅ correctness only, not hashrate |

---

## 5. AI / ONNX — feasible, but the wrong tool for control

**ONNX Runtime does not exist for ESP32-S3.** `espressif/esp-onnxruntime` → 404.
The only Espressif AI repos are `esp-dl`, `esp-nn`, `esp-tflite-micro`. ONNX
appears in ESP-DL only as the **import format for the offline quantiser**
(`ONNX Runtime==1.7.0` there is a desktop Python dependency of the calibration
tool). Anyone claiming to run onnxruntime-micro on an S3 is describing something
that does not exist.

TinyML itself is entirely feasible: esp-dl/esp-nn have first-class S3 support with
published numbers (person detection 2300 ms → **54 ms** with ESP-NN; ESP-DL SIMD
conv2d speedups **26–77×**). A control model here is a 3–4 layer MLP, 1–3 k
params, int8, **1–5 KB** weights, **sub-millisecond** inference. Inference cost is
irrelevant; it is not the constraint.

**But ML closed-loop control is the wrong choice, and I argue against it:**

1. **We already have the reference architecture.** Braiins' beta Continuous Tuner
   describes itself as a *"predictive model … estimates thermal headroom …
   anticipates thermal limits rather than waiting until they're hit"*, recomputed
   every ~5 min — that is receding-horizon MPC by another name. Their shipped
   DPS is explicitly *reactive*. Our ATM (hysteresis + DPS memory + regulator-floor
   clamp + minimum dwell) is already at or above what the dominant open-source
   firmware ships, **and it is validated on our hardware.**
2. **The physics is not what ML is good at.** First-order lag, known actuator,
   monotone one-sided constraint. The MPSoC thermal-control literature is
   unanimous that model-free strategies are *"completely immune to uncertainties"*
   while MPC *"must trade off model complexity against the safety margin"*, and
   that RL methods *"lack formal feasibility guarantees, especially in case of
   model uncertainties."*
3. **Failure modes are wildly asymmetric.** A 1 % hashrate gain is worth nothing.
   A controller that under-clocks to 900 mV for four minutes is silicon damage.
   Worse, an MLP that misbehaves fails **silently**; a hysteresis controller that
   misbehaves is visible on a temperature graph.
4. **Our uncertainty is the sensor, not the model.**

**The one AI use case I would defend:** a tiny model for **sensor-fault /
bad-reading detection** — which we genuinely need (LuxOS ships `Max Bad Readings`
= 10 and `Bad Avg Threshold` = 2; we have neither), and a 1-D CNN over a
temperature series is a legitimate ESP-NN workload.

**Right division of labour: ML for prediction, rules for control.**

---

## 6. Control improvements worth stealing (no ML required)

Ranked by value ÷ risk:

1. **Power-targeting loop.** LuxOS Power Targeting and Braiins both anchor the
   loop on a measurable *outcome* (watts / power target) rather than a
   temperature setpoint. LuxOS measures **>10 % diurnal swing in power draw at
   fixed settings** from ambient alone — *that swing is the entire prize*, and a
   fan-PID loop cannot even see it. Concretely: *"hold N watts; find the lowest
   voltage and the frequency that delivers ≥95 % of expected hashrate."* This
   subsumes our V/F tuner and makes the "safe vs unsafe voltage" question moot,
   because the constraint becomes power rather than a voltage guess.
2. **1 °C/min temperature-stability gate on upscale.** Braiins requires *stable
   temperature — "did not fluctuate more than 1 °C in the last minute"* before
   allowing an upscale. We have no such gate. ~20 lines, directly prevents
   overshooting a still-transient excursion, which is exactly when the old
   controller hunted.
3. **66 %-of-target startup derate.** Braiins v26.01+: if chip temp reaches Hot
   within the **first 5 minutes**, drop to 66 % of target; after that, one step.
   A startup thermal transient is a distinct regime and deserves a distinct
   policy. We have none.
4. **RLS online system identification** — if we want genuine prediction without
   ML, this is the technique: recursive least squares learns *this board's* actual
   thermal transfer function. O(n²) with n≈3, trivial on an S3, and it yields a
   real model instead of a guessed one. Published precedent on a low-cost MCU:
   [S0967066120302446].
5. **Critical-sensor quorum.** LuxOS enforces a minimum count of valid
   temperature sensors and rejects bad readings. A Bitaxe has 2 ASIC sensors +
   VR + regulator; if one dies the controller currently reads garbage silently.

⚠️ There is effectively **no peer-reviewed MPC work on bitcoin ASIC miner
thermal control**; the literature is MPSoC/datacentre. Transfer is by analogy.

---

## 7. Pool / coin economics — do the arithmetic before optimising

* **Variance is irreducible at one machine.** Block arrivals are Poisson, so
  variance grows as √mean while expectation grows as mean. At ~430 TH/s solo the
  coefficient of variation is **~55 %, and no pool choice changes it** — pools
  change fee and cadence, not variance.
* **PPS is provably variance-minimal** (FC 2021, *Ignore the Extra Zeroes*:
  *"Single-class shares are optimal … Pay-per-share is optimal"*), and PPLNS is
  optimal under no-deficit constraints. The maximum fee worth paying for
  smoothening is *"extremely small"* (arXiv:2309.02297). ⇒ **At our size a 2 %
  PPS+ fee beats a 0.5 % PPLNS pool**, provided the payout threshold actually
  triggers.
* **Optimise reliability and payout threshold, not fee.** Fee differences are
  cents on a ~$1 block. What matters: does the pool actually find blocks, how low
  is the threshold, how reliable is the endpoint. ⚠️ A pool holding >4 PH/s that
  has **never found a DigiByte block** is telling you something.
* **Target one share per 10–30 s.** Below 10 s we burn ESP32 CPU verifying and
  logging shares for no revenue gain (upstream #1955's argument); above ~60 s
  granularity and fault-recovery windows get bad.
* **DGB is structurally the right coin.** Five algorithms each target ~1/5 of
  blocks, so a SHA-256 miner competes only against other SHA-256 miners, and that
  network is in the PH/s range — we are a meaningful fraction of it, not a
  rounding error. ⚠️ **But** at ~$1/block, solo reward plausibly does not cover
  20–25 W of electricity. **Mine for the share market, not the block lottery**,
  and verify the tariff arithmetic before optimising anything else.
* ⚠️ zpool's profit index shows DGB paying ~45 % less per unit hashrate than the
  best SHA-256 multipool entry — but that is a **scraped instantaneous estimate**
  that ignores variance, payout threshold and reliability, and altcoin spreads
  have historically been arbitraged away. **Treat it as a prompt to run our own
  7-day realised-revenue comparison on 2–3 coins, not as a decision.**
* **Preference: a single-coin pool** over a multipool. Multipools are inherently
  adversarial to a small miner — no control over which coin you are paid in, and
  *"non-guaranteed payouts are paid from coins mined at pool"*. At our absolute
  revenue, **certainty is worth more than a marginal rate.**

---

## 8. Proposed execution order

| # | item | risk | depends on |
|---|---|---|---|
| 1 | Log the notify-drop **reason** (§1.2a) | none | — |
| 2 | `force_clean_pending` (§1.2b) | low | 1 |
| 3 | Verify/`set TCP_NODELAY` (§1.3) | none | — |
| 4 | Pin pool difficulty (§1.3) | none (config) | — |
| 5 | Faster ladder + free rung-1 entry (§2) | medium | — |
| 6 | Log requested vs applied frequency (§1.5) | none | — |
| 7 | Cherry-pick #1926, #1855 (§1.4) | medium | — |
| 8 | Temperature-stability gate + startup derate (§6.2–6.3) | low | — |
| 9 | `MAIN_TASK_STACK_SIZE` then `MALLOC_ALWAYSINTERNAL` (§3) | low | — |
| 10 | Power-targeting loop (§6.1) | high | 8 |
| 11 | Re-measure stale rate over ≥4 h | — | 1–7 |

**Gate:** no further work until the ≥4 h measurement in §1.6 lands. If stale is
still >5 %, the stranding theory is wrong and items 2/7 are the wrong lever.

## 9. Explicitly rejected

| idea | why |
|---|---|
| ASIC Boost work | already in the silicon; no firmware lever exists |
| "LuxOS Firmware Reject" | does not exist in any primary source |
| ONNX Runtime on S3 | no such port exists |
| ML closed-loop thermal control | no feasibility guarantee, silent failure mode, strictly worse than the validated ATM |
| Shortening `default_asic_timeout` | host-side send cadence only; cannot make a job fresher; increases slot churn |
| Reducing `QUEUE_SIZE` | not exercised — every observed notify had `clean_jobs=true`, which drains the queue each time |
| Globally PSRAM task stacks | NVS/OTA run with cache disabled and need internal-RAM stacks |
| Chasing `diff 467 of 416` | those are **passing** shares; a too-easy share would be error 23, not 21 |
| Re-applying upstream #1424 "Go Queueless" | already merged upstream 2026-01-21 and superseded |

---

## 10. Sources

- AsicBoost paper: https://arxiv.org/abs/1604.00575
- ckpool error table (`21 = Job not found (=stale)`): https://github.com/ckolivas/ckpool/blob/master/src/libckpool.h
- node-stratum-pool job manager (retains exactly one job): https://github.com/s-nomp/node-stratum-pool/blob/master/lib/jobManager.js
- Upstream #1926 job-interval clamp: https://github.com/bitaxeorg/ESP-Miner/pull/1926
- Upstream #248 job interval for version-rolling ASICs: https://github.com/bitaxeorg/ESP-Miner/issues/248
- Upstream #420 nonce space / timeouts: https://github.com/bitaxeorg/ESP-Miner/pull/420
- Upstream #1722 `TCP_NODELAY`: https://github.com/bitaxeorg/ESP-Miner/issues/1722
- Upstream #1855 source epochs: https://github.com/bitaxeorg/ESP-Miner/pull/1855
- Upstream #1989 PLL + applied-frequency nonce timing (open): https://github.com/bitaxeorg/ESP-Miner/pull/1989
- Upstream #1864 frequency clamp / `pll_get_parameters` no error return: https://github.com/bitaxeorg/ESP-Miner/pull/1864
- Upstream #1044 PLL lookup table investigation: https://github.com/bitaxeorg/ESP-Miner/issues/1044
- Upstream #1955 dynamic ticket mask: https://github.com/bitaxeorg/ESP-Miner/pull/1955
- Upstream #1806 flatline-of-death watchdog: https://github.com/bitaxeorg/ESP-Miner/pull/1806
- Upstream #203 ~10 % shares rejected on public-pool: https://github.com/bitaxeorg/ESP-Miner/issues/203
- Upstream #167 BM1366 nonce-coverage unit test: https://github.com/bitaxeorg/ESP-Miner/pull/167
- LuxOS ATM settings: https://docs.luxor.tech/firmware/features/atmsettings
- LuxOS AutoTuner / power targeting: https://docs.luxor.tech/firmware/introduction-to-firmware
- Braiins DPS (incl. DPS Memory, stability gate, startup derate): https://academy.braiins.com/braiins-os/dps
- Braiins continuous/predictive tuner (beta): https://academy.braiins.com/braiins-os/continuous-tuning
- ESP-IDF external RAM / `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/external-ram.html
- ESP-IDF flash/PSRAM combination table: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/flash_psram_config.html
- esp-dl (S3 supported targets): https://github.com/espressif/esp-dl
- esp-nn (published S3 kernel speedups): https://github.com/espressif/esp-nn
- esp-tflite-micro: https://github.com/espressif/esp-tflite-micro
- Variance-optimal mining pools (PPS optimal), FC 2021: https://fc21.ifca.ai/papers/171.pdf
- Smoothening block rewards — max fee worth paying: https://ar5iv.labs.arxiv.org/html/2309.02297
- Albrecher/Finger/Goffard, empirical risk analysis of PoW mining: https://exa.ai/library/publication/5v5vrhrwskh
- Mining pools: theory and practice (Chatzigiannis et al.): https://academic.oup.com/cybersecurity/article/8/1/tyab027/6550812
- Poisson variance in solo mining: https://solofury.com/blog/mining-variance-poisson-math/
- RLS general predictive control on a low-cost MCU: https://www.sciencedirect.com/science/article/abs/pii/S0967066120302446
- Proactive fan DVFS via thermal history (MPSoC): https://re.public.polimi.it/retrieve/handle/11311/1129453/489459/
- MPC vs model-free trade-off for thermal control: https://cris.unibo.it/retrieve/handle/11585/868929/