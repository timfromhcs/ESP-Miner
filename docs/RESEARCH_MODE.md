# SHA research mode (experimental)

This document describes the optional on-device SHA research surface that the
`feat/timsminer-sha-research-mode` branch adds to ESP-Miner. It is **off by
default** and does nothing unless the firmware is built with
`CONFIG_TIMSMINER_SHA_RESEARCH=y`.

Read this before enabling it on a miner that is actually hashing.

## 1. What it is, and what it is not

It is:

- A software SHA-256 / SHA-256d implementation on the ESP32-S3 that can return
  the **per-round values of that software computation** for a 64-byte block the
  caller supplies.
- An independent oracle. The same C file
  (`main/research/research_sha.c`) is compiled into the firmware and into the
  host test binary (`simulation/test_research_sha.c`), so a client and the device
  can check each other. The emitted trace is
  `hcs.sha.trace.v1` / `SOFTWARE_REPLAY`, byte-compatible with
  `cqhecs/sha/round_trace.py`; `cqhecs.sha.round_trace.replay()` accepts a device
  trace unchanged.
- A small, bounded set of named commands with an explicit authorisation model.

It is not:

- An ASIC round analyzer. The BM1366 executes SHA-256d internally and returns
  nonces over the UART. There is no readable round state, no writable message
  block, no arbitrary IV and no "round" register. Nothing in this feature reads
  or writes the ASIC. `GET /api/research/capabilities` says so in its own
  payload (`limitations.asic_access = "none"`).
- A mining accelerator. Research jobs compete with the mining task for CPU time.
- A remote shell. There is no register-write endpoint and no arbitrary command.

## 2. Build

```bash
# from the ESP-Miner repository root
idf.py menuconfig        # HCS SHA Forge research -> enable, set budgets
# or, for a per-device build, add to the variant's sdkconfig:
#   CONFIG_TIMSMINER_SHA_RESEARCH=y
#   CONFIG_TIMSMINER_SHA_RESEARCH_MAX_JOBS=64
#   CONFIG_TIMSMINER_SHA_RESEARCH_MAX_RUNTIME_S=900
#   CONFIG_TIMSMINER_SHA_RESEARCH_TEMPERATURE_C=65
idf.py build
```

The Kconfig ceiling can only *tighten* the device's own thermal protection. The
guard rejects a configured value above 70 °C, and the existing power-management
and thermal tasks keep ownership of overheat handling, fan control and shutdown.

Host tests (no ESP-IDF, no device):

```bash
cd simulation
make test_research_sha test_research_guard
./test_research_sha        # FIPS 180-4 KAT, padding, genesis SHA-256d, trace invariants, JSON
./test_research_guard      # every refusal path of the authorisation guard
```

CI runs both, including under ASan/UBSan, in the `host-tests` job.

## 3. Authorisation model

A research session exists only inside these constraints:

| Gate | Enforced by | Refusal reason |
|---|---|---|
| Build flag | Kconfig | routes do not exist |
| Short-lived operator token | `research_guard_arm()` | `no_valid_token` |
| Device identity verified this boot | `sample_conditions()` | `identity_unverified` |
| No pool connected, no share outstanding | `sample_conditions()` | `pool_active` |
| Chip temperature known and below the ceiling | `sample_conditions()` | `thermal_unsafe` |
| Measured power below the ceiling (when measured) | `research_guard` | `power_unsafe` |
| Job budget | `research_guard` | `job_budget_exhausted` |
| Session time budget | `research_guard` | `time_budget_exhausted` |

Unknown is unsafe: a missing thermal reading, a NaN or a negative sensor value
disarms the session rather than being ignored. Unknown *power* is the single
documented downgrade — it is reported as `power_verified: false` in the status
instead of blocking, because this board's thermal path is the operative limit.

The state machine is `disarmed → armed → running → armed | paused`, and every
unsafe condition or exhausted budget returns to `disarmed`. Re-arming always
requires a fresh token: authorisation is not a durable property of the device.

### Pairing token

`POST /api/research/challenge` generates six digits from the hardware RNG and
prints them to the **serial log**, not to the HTTP response:

```
W (12345) research_api: research pairing token: 482913 (valid for 120 s, read it on the serial console)
```

The operator reads that line on the USB console and posts it back to
`/api/research/arm`. This is a speed bump against a stray request, not strong
authentication: a client that can already read the device's log endpoint (or the
serial line) can complete the pairing. Treat the research API as a same-LAN,
single-operator surface; do not expose the device to the internet. A failed arm
does not burn the token, so one wrong guess cannot invalidate the operator's
pending challenge.

## 4. Endpoints

All are `application/json`, CORS-enabled, and restricted by the existing
`is_network_allowed()` check (private ranges only).

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/research/status` | typed status document (`hcs.sha.miner.v1` + `kind: research_status`); also runs the watchdog |
| GET | `/api/research/capabilities` | what this build supports and, explicitly, what it does not |
| POST | `/api/research/challenge` | issue a pairing token to the serial log |
| POST | `/api/research/arm` | `{"token":"482913"}` |
| POST | `/api/research/pause` | running → paused |
| POST | `/api/research/resume` | paused → armed (the abandoned job is not resumed) |
| POST | `/api/research/cancel` | abandon the current job, keep the session |
| POST | `/api/research/disarm` | end the session now |
| POST | `/api/research/sha256` | the oracle |

### `POST /api/research/sha256`

Block trace (streamed, one JSON document):

```json
{"block_hex":"61626300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
 "rounds":64,
 "feed_forward":true}
```

`rounds` accepts 0–64 so a caller can inspect a truncated schedule; `rounds:0`
with `feed_forward:true` returns input + input, exactly as the Python reference
does. The response is:

```json
{"schema":"hcs.sha.device_sha256.v1","kind":"block_trace","compute":"software_cpu",
 "asic_access":"none","firmware_version":"...","trace":{ "schema":"hcs.sha.trace.v1", "...": "..." }}
```

The `trace` object is the unmodified `hcs.sha.trace.v1` document. Provenance
(`compute`, `asic_access`, firmware) is in the envelope, never inside the trace:
`replay()` compares round objects field by field and an extra key would make a
correct trace look tampered with.

Message digest (single response):

```json
{"message_hex":"616263","sha256d":true}
```

→ `{"schema":"hcs.sha.device_sha256.v1","kind":"message_digest","algorithm":"sha256d","message_bytes":3,"digest":"..."}`

Limits: block must be exactly 64 bytes; message at most 512 bytes; body at most
2 KiB. The oracle refuses (`409`) while a pool is connected, while a share is
outstanding, when identity is unverified, or when the chip is at/above the
research ceiling. It does not require an armed session — it is a bounded,
read-only computation — but it never competes with an active share.

## 5. Operator flow (mining stops first)

Research work is refused while a pool is connected, so a session looks like this:

```bash
HOST=<device>            # never committed
curl -s -X POST "http://$HOST/api/system/pause"            # stop mining (stock route)
curl -s -X POST "http://$HOST/api/research/challenge"      # token appears on the serial log
curl -s -X POST "http://$HOST/api/research/arm" -d '{"token":"<6 digits>"}'
curl -s -X POST "http://$HOST/api/research/sha256" \
     -d '{"block_hex":"<128 hex>","rounds":64,"feed_forward":true}' > trace.json
curl -s -X POST "http://$HOST/api/research/disarm"
curl -s -X POST "http://$HOST/api/system/resume"           # mining returns
```

`/api/system/pause` and `/api/system/resume` already exist in ESP-Miner; this
feature does not add or change them. The research routes never resume mining
themselves — that is always an explicit operator action.

## 6. Honest measurement semantics

If you benchmark this, label it as a **software** measurement:

- `sha256_compressions_per_s` — software compressions of this C implementation.
- `asic_hashrate_ghs` — the BM1366's own mining hashrate, a different quantity,
  and not comparable on the same axis without a conversion note.
- There is no `asic_domain_counter_delta` in this build. The ASIC error counter
  documented in the ESP-Miner deep dive is not a hash counter and is not exposed
  here as one.

## 7. Failure modes

| Symptom | Cause | Response |
|---|---|---|
| `409 wrong_state` on arm | already armed/running | disarm first |
| `409 no_valid_token` | token expired (default 120 s) or already used | request a new challenge |
| `409 pool_active` | the miner reconnected between arm and job | the guard already disarmed the session |
| `409 thermal_unsafe` | reading unknown or above the ceiling | let it cool; do not raise the ceiling |
| trace response ends mid-document | client disconnected | the writer aborts the computation; the socket close is the signal |
| `503` on every route | system state not initialised yet | retry after boot |

The device never reboots, never changes a partition and never touches mining or
pool configuration from these endpoints.
