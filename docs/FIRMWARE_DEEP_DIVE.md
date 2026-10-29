# Firmware deep dive

How this fork differs from upstream, the measurements behind the claims, the hardware and the
architecture. Moved here from the README so the README stays an entry point. Each claim keeps
the label it had there; see the [evidence table](../readme.md#evidence--truth-classification) for the summary.

## What is actually different in this fork

The dominant failure mode in mining firmware is **code that only runs on its error path**. On a healthy miner the error path is never taken, so the defect is invisible in normal operation and surfaces as a symptom with no obvious cause. Three of the highest-impact fixes below were exactly that shape, and two of them had been shipping for months.

### AsicBoost (BIP310 version rolling)

AsicBoost here is **overt** version rolling: the miner asks the pool for it in `mining.configure`, the pool replies with a mask, and the chip sweeps the version itself. Nothing covert, no claim of unverified silicon behaviour.

The rollable window is **bits [13:28]** of the block header's version field — 16 bits. Everything above bit 28 is BIP9 signalling and is not rollable. Four defects sat around that window:

1. **The BM1370 had no version-roll bookkeeping to speak of and the wrong host/chip agreement.** Register `0xA4` is only 16 bits wide, so the roll count is `mask >> 13` and a larger mask does not fit. This firmware asks the pool for `"version-rolling.mask":"ffffffff"`; a pool that grants it returns `0xFFFFFFFF`, i.e. 0x1FFFF rolls. The old code narrowed the high byte into a `uint8_t`, which silently dropped the 17th bit — the chip received 0xFFFF while the host believed 0x1FFFF and logged that number. Now clamped explicitly through a single definition (`ASIC_VERSION_ROLL_MASK`), so host and chip cannot disagree.

2. **The submitted version bits were recovered by XOR.** `rolled_version ^ job->version` only yields the rolled bits while the pool's version field happens to have none of them set — and BIP9 signalling occupies bits [25:27], inside the rollable window. Against such a pool the XOR cancelled exactly those bits and the share went out with a version the pool reconstructs differently: a rejection with no local symptom. Now masked with the roll range, which is correct either way.

3. **Chip configuration could wait a whole block cycle.** The handlers for `mining.set_difficulty` and `mining.set_version_mask` sat inside the `if (new_work != NULL)` branch of the job loop. A change arriving while the queue was empty — or on an SV2 standard channel, where work is never re-dequeued at all — was not applied until the next `mining.notify`. On the pool these units mine against, that is a full block. Moved ahead of the blocking dequeue; the pending flags still wait for the ASIC to actually be up.

4. **Three midstates were computed per job and never transmitted.** `construct_bm_job()` produced `midstate`/`midstate1`/`midstate2`/`midstate3` whenever a mask was set, but only the BM1397 job frame can carry more than one — the other four drivers hardcode `num_midstates = 0x01`. That is three wasted SHA-256 compressions and 96 bytes of dead payload per job, plus a `num_midstates = 4` claim that is wrong for four of five ASIC families. Now gated on `ASIC_family_uses_rolled_midstates()`, with tests for both branches and a golden check that the rolled midstate matches an independent recomputation.

### UART integrity

Register `0x28` selects the chip's baud rate, which it derives from a 25 MHz on-chip reference:

```
baud = 25 000 000 / ((BT8D + 1) * 8)
```

- **The BM1370 wrote BT8D = 2 but returned 1 000 000 to the host** — a systematic 4.17 % mismatch, with the receiver sampling the wrong bit grid. This exact defect was found, documented and fixed on the BM1366 in v2.16.0 and never carried over. It matters considerably more on the Gamma, which sustains roughly 3.5× the hashrate of an Ultra over the same link.
- **A zeroed PLL divider set is never written.** No valid solution for the requested frequency means register `0x08` is left alone; writing zeros leaves the PLL unlocked and the chip idle until a reboot.

### The ticket mask — where a mining firmware quietly wastes its CPU

Register `0x14` decides when the chip forwards a nonce. A mask of *N* means roughly `hashrate / N` nonces per second on the wire. **Every** nonce that arrives costs the ESP32 a full double-SHA256 in `test_nonce_value()` before it is either submitted or discarded, so an under-set mask costs real CPU, not just log volume.

The BM1366 driver already tracked the live pool difficulty. **The BM1370 driver had no runtime hook at all** — `BM1370_init()` wrote the register twice, both times from the static family default of 256, and never touched it again. On a ~1.5 TH/s chip a 256 mask asks for several million reports per second across a 1 Mbaud link, and the mask was simply inherited from the much slower Ultra. It is now seeded from the pool difficulty at init and re-programmed on every `mining.set_difficulty`.

### Observability that was switched off

- **Notify counters and the pool clock sync only ran on the coinbase-decode-failure path.** `notify_received`, `work_received` and `SYSTEM_notify_new_ntime()` were all incremented inside that one `if` block. A miner whose notifies decode cleanly — that is, a *working* miner — therefore reported `notifyReceived: 0`, `workReceived: 0`, `notifyDropped: 0` for the entire session and never called `settimeofday()` at all. Confirmed on all three fleet members before the fix. Stratum V2 already did this correctly. The counters exist precisely to make `received + dropped == well-formed notifications` observable, and a unit reporting three zeros is indistinguishable from a pool that stopped sending jobs.
- **A per-nonce `ESP_LOGI` was drowning the log ring.** `asic_result` logged one INFO line per reported nonce — thousands per second on a Gamma. The 128 KB ring buffer, and with it the entire boot trace and any real warning, was overwritten within seconds. **A device in a bad state consequently looked clean in its own logs.** Demoted to `ESP_LOGD`. This one change is what made the ticket-mask defect diagnosable at all: the boot trace only became readable after it.

### Networking

- **Deterministic DHCP lifecycle.** The client start is bound to 802.11 station association and only issued when the client is actually stopped — no `stop`→`start` bounce, which resets the DHCP xid. A valid lease is never wiped on re-association, retries use jittered backoff, and the station re-associates after three exhausted cycles until a real lease arrives. Lease conflicts are left to the DHCP client via `CONFIG_LWIP_DHCP_DOES_ARP_CHECK`, which is deliberately *not* ACD, because ESP-IDF documents that ACD "can cause repeated DHCP DECLINEs" on access points that echo the client's MAC in ARP replies — exactly what a FRITZ!Box does.
- **Operator-configured static fallback, off by default.** If DHCP genuinely fails there is an optional last-resort static IPv4 address: stored per-device in NVS, editable in AxeOS, applied only after the retry cycles are exhausted. There is deliberately **no site-specific default** — a default naming a specific address would make every unit built from this firmware claim an address it was never given, which is precisely how duplicate-IP conflicts start. Per-device builds bake in their own values.
- **The recovery ladder is unit-tested, not just described.** It was lifted out of `connect.c` into `net_recovery_policy.c` so the rungs — lease replay, resolver preference, fallback binding, and the rule that the fallback must not fight a running DHCP client — can be exercised on the host (92 checks in `v4.0.2-rc1`). Since `v4.0.2-rc1` the station also never stops reconnecting: a handshake timeout while a rebooting router brings WPA back used to end all retries until a power cycle - see [Network recovery design](NETWORK_RECOVERY_DESIGN.md).

### Memory

Two independent defects produced identical reboot loops and both are fixed and verified on hardware:

- **A silent 512 KB buffer in internal DRAM.** `main/log_buffer.c` declares its ring `EXT_RAM_NOINIT_ATTR`, which is *conditional*: unless `CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY` is set, ESP-IDF silently degrades it to `__NOINIT_ATTR` and the buffer lands in internal `.noinit`. The ESP32-S3 caps `dram0_0_seg` at 341 KB, so the link failed with `region 'dram0_0_seg' overflowed`. `main/CMakeLists.txt` now aborts the build with an explicit message if either PSRAM segment option is not `y`, because `sdkconfig` is not versioned and a drifted local copy reproduced this on *every* commit. Internal DRAM reserve went from −363 KB to +176 KB. The ring was also right-sized from 512 KB to 128 KB.
- **Idle task stack.** `CONFIG_FREERTOS_IDLE_TASK_STACKSIZE` sat at the IDF default of 1536 B; on this board Wi-Fi coexistence and the scheduler run in the idle context and the canary was overrun reproducibly at ~2.3 s into boot. Raised to 4096 B.
- **Overload protection** watches the **largest free internal block**, not total free heap, because FreeRTOS allocates task stacks as one contiguous block — 40 KB of fragmented free heap is as unusable for a 4 KB stack as an empty heap. It warns below 24 KB and only after 3 consecutive critical samples does it perform a controlled `esp_restart()`.

### Undecodable jobs

A `mining.notify` whose coinbase cannot be decoded is **kept and mined**. The coinbase is parsed only to show the reward breakdown in the UI; the work itself comes from the job id, prevhash and merkle root, all of which are valid. Dropping such a job costs far more than the missing UI detail: the pool has already retired the job and issues a new `job_id` only every ~60 s, so every share submitted before the next notification is rejected as stale. Measured at 12–14 % rejection on a pool with an unhandled coinbase layout, with not one share missing from the coinbase detail.

What *is* refused before anything is allocated is a structurally impossible job: `notify_validate.c` checks the job id, prev-block hash, both coinbase halves, every merkle branch and the compact target against the same rules Bitcoin Core uses in `arith_uint256::SetCompact`. The compact-target bounds were wrong at both ends — they accepted exponents Core classifies as a 256-bit overflow and rejected exponents Core accepts — and are now differential-tested against an independent reference decoder over 20 000 random words plus an exhaustive exponent sweep.

---

## Measurements

### Host efficiency — same unit, before and after

Bitaxe Gamma `192.168.178.80`, 750 MHz, 1.195 V, mining Zpool SV1. Two 400 s
captures at 5 s resolution via [`tools/collect_telemetry.ps1`](../tools/collect_telemetry.ps1),
taken on the same hardware with no setting changed. Rates are the delta between
first and last sample, so they do not depend on the boot counters.

| Metric | `v4.0.0` before | after the fixes | Δ |
|---|---|---|---|
| Hashrate (1 h avg) | 1528.9 GH/s | 1516.4 GH/s | −0.8 % |
| **ESP32 CPU load** | 15.87 % | **14.10 %** | **−11.2 %** |
| **Share submit latency** (`processTime`) | 2.954 ms | **2.495 ms** | **−15.5 %** |
| Effective share rate (difficulty-normalised) | 18 018 diff·/min | 19 124 diff·/min | +6.1 % |
| ASIC error register 0x4C | 1.69 /s | 1.83 /s | within noise |
| `notifyReceived` / `workReceived` | **0 / 0** | 6 / 6 in 400 s | fixed |
| Rejected shares | 0 | 0 | — |

The two bold rows are the result the ticket-mask fix predicts and they are the
ones with a mechanism behind them. A chip masked at 256 while the pool asks for
2210 makes the ESP32 verify roughly nine times more hashes than it keeps; once
the mask tracks the pool, that work disappears. CPU load and submit latency both
drop together, which is what a reduction in wasted per-nonce SHA-256 work looks
like.

### Stale-share parity against the reference units

A stale rejection (`Invalid job id`, pool error 21) is the job-boundary race: a
nonce already in flight when the pool retires the job. It is not fully avoidable,
so the useful question is whether a device produces it at the *inherent* rate or
at a rate proportional to its own speed.

| Unit | ASIC | Hashrate | Uptime | Shares | Stale | Stale / h | Stale % |
|---|---|---|---|---|---|---|---|
| `timsminer` `.61` | BM1366 | 433 GH/s | 353 h | 185 918 | 558 | 1.581 | 0.299 % |
| `blackharkminer` `.66` | BM1366 | 433 GH/s | 353 h | 189 101 | 550 | 1.558 | 0.290 % |
| `bitaxe-tim2` `.80` | **BM1370** | **1531 GH/s** | 14.9 h | 7 630 | 24 | **1.614** | **0.314 %** |

The Gamma runs at 3.5× the hashrate of the two units that have been validated for
two weeks, and produces stale shares at the **same absolute rate**. A
device-specific defect would scale with the device's speed; this does not, so the
remaining ~0.3 % is the pool-side latency race and not something the firmware
here can remove.

### Sustained run (`v4.0.1-stable-tim2`)

`bitaxe-tim2` ran `v4.0.1-stable-tim2` continuously after its OTA: 14.9 h,
7 630 shares accepted, 24 stale, **0 rejected for any other reason**, die
temperature held at 59–60 °C, VR temperature 74–77 °C, internal DRAM free heap
flat at ~78.9 kB with no drift, `notifyReceived` tracking notifies continuously
with `notifyDropped` at 0.

Energy efficiency on the Gamma is **18.8 J/TH** against ~25 J/TH for the BM1366
Ultras, which is what the newer silicon is expected to do.

### A caveat on `errorPercentage`

The API's `errorPercentage` is derived by treating ASIC register `0x4C` as a
hash counter and dividing by total hashrate. That register is an error *count*,
not a hash count, so the resulting percentage is not dimensionally meaningful —
it moves with the hashrate even when the underlying counter does not. The honest
metric is the raw register delta, and that one is flat at ~1.7–1.9 per second
across every run on this device. The `0.00 %` reported by the two BM1366 units
means their error register never advanced, not that they are more accurate.
Treating `errorPercentage` as a real figure is left as an open item rather than
claimed as fixed.

Raw captures, device logs and the firmware hash are in
[`evidence/192.168.178.80/`](../evidence/192.168.178.80/).

---

## Hardware

| Component | Bitaxe Ultra (Board 201) | Bitaxe Gamma (Board 601) |
|---|---|---|
| MCU | ESP32-S3 dual-core Xtensa LX7 @ 240 MHz | same |
| ASIC | 1× BM1366, 112 cores, 894 small cores | 1× BM1370, 128 cores, 2 040 small cores |
| Hash domains | 4 | 4 |
| Verified operating point | 485 MHz @ 1.200 V | 750 MHz @ 1.195 V |
| Flash | 16 MB SPI (GD25Q128E, QSPI @ 80 MHz) | same |
| PSRAM | 8 MB Octal SPI | same |
| Core voltage | TPS40305 buck + DS4432U+ I2C DAC | TPS546 |
| Thermal | EMC2101 + external diode | EMC2101 + external diode |
| Power telemetry | INA260 | INA260 |
| Display | SSD1306 128×32 OLED | SSD1306 128×32 OLED |

---

## Architecture

```
┌────────────────────────────────────────────────────────────────────────┐
│                              ESP32-S3                                 │
│  ┌──────────────────────┐             ┌─────────────────────────────┐  │
│  │   Axe-OS Web App     │             │     Stratum V1 / V2 Client  │  │
│  │   (Angular SPA)      │             │   - JSON-RPC / Noise        │  │
│  └──────────┬───────────┘             └──────────────┬──────────────┘  │
│             │ HTTP REST / WebSocket                  │ Jobs            │
│  ┌──────────▼───────────┐             ┌──────────────▼──────────────┐  │
│  │     HTTP Server      │             │      Create Jobs Task       │  │
│  └──────────────────────┘             │  - coinbase / merkle /      │  │
│                                       │    midstate (+ version roll)│  │
│                                       └──────────────┬──────────────┘  │
│                                                      │ 1.04 MBaud UART  │
│  ┌──────────────────────┐             ┌──────────────▼──────────────┐  │
│  │ EMC2101 Thermal PID  │             │      BM13xx ASIC Driver     │  │
│  │  - Closed-loop PWM   │             │  - 0xA4 version rolling     │  │
│  │  - 500ms sample rate │             │  - 0x14 ticket mask         │  │
│  │                      │             │  - 0x28 UART rate           │  │
│  └──────────┬───────────┘             │  - CRC5 frame check         │  │
└─────────────┼─────────────────────────┴──────────────┬─────────────────┘
              │ I2C                                    │ UART
┌─────────────▼───────────┐             ┌──────────────▼──────────────┐
│ EMC2101 / INA260 / DAC  │             │      1× BM13xx ASIC         │
└─────────────────────────┘             └─────────────────────────────┘
```

Further reading: [Architecture](ARCHITECTURE.md) · [Networking](NETWORKING.md) · [ASIC subsystem](ASIC.md) · [Job planning](JOB_PLANNING.md) · [Memory](MEMORY.md) · [Performance](PERFORMANCE.md)

---
