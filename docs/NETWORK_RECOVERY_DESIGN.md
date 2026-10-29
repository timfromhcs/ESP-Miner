# Network Recovery Design

Complements `docs/NETWORKING.md` (operator view). This file states the ownership
rules, the retry schedule actually implemented in `v4.0.2-rc1`, and which claims
are tested. Labels per `GEMINI.md` §0.4.

## 1. Layers and owners

| Layer | Owner | Recovery action | Must not |
|---|---|---|---|
| Wi-Fi association | `connect.c` event handler + `s_wifi_retry_timer` (esp_timer) | re-associate after `net_policy_wifi_retry_delay_ms()` | stop retrying; block the event loop |
| DHCP / IPv4 | `ip_timeout_callback` (FreeRTOS timer) | escalation ladder: retry → bounce client (fresh xid) → re-associate; last-lease / operator fallback | stop the DHCP client to bind a guessed address |
| Fallback latch | `net_recovery_policy.c` (`net_policy_should_reassert`, `net_policy_lease_wins`) | rebind last real lease; cleared by any real lease | persist its own resolver as a lease |
| DNS | per pool connect (`stratum_socket_resolve`) | retried with the pool connect | reboot Wi-Fi |
| Pool transport | `stratum_v1_task` / `stratum_v2_task`, `protocol_coordinator` | reconnect, then failover | reset the ASIC |

Static fallback is **off** in the general build (`ESP_MINER_FALLBACK_ENABLED=0`,
empty address; `main/nvs_config.h`). Only per-device builds
(`tools/build_release.sh <variant>`) enable it, with an address reserved on the
router for that unit's MAC. PROVEN (code + `CMakeLists.txt` defaults).

## 2. Station reconnect schedule

`net_policy_wifi_retry_delay_ms(attempt, auth_failure, jitter)`:

| Condition | Delay |
|---|---|
| ordinary disconnect, attempt *n* | `min(1000 + 800·min(n,10) + jitter%700, 8000)` ms |
| handshake-type reason (`AUTH_FAIL`, `4WAY_HANDSHAKE_TIMEOUT`, `HANDSHAKE_TIMEOUT`, `AUTH_EXPIRE`) and attempt ≥ 3 | `30000 + jitter%5001` ms |

There is no terminal state. `attempt` is reset only by `IP_EVENT_STA_GOT_IP`.
The config AP is raised after more than 10 attempts or with no SSID (unchanged).

**Why:** up to `v4.0.1-stable` the handshake-type case returned without
scheduling another connect. A router reboot produces a run of `NO_AP_FOUND`
followed by a handshake timeout while WPA comes up, so a unit could stay offline
after the router returned until power-cycled. PROVEN (code reading: no other path
called `esp_wifi_connect()`); the new schedule is TESTED
(`test_wifi_retry_never_gives_up`, 100 000 attempts × 5 jitter values, overflow
and negative inputs). Physical reproduction: NOT MEASURED.

The delay is an `esp_timer` one-shot rather than `vTaskDelay()` in the event
handler, which had held all Wi-Fi/IP events for up to 8 s per disconnect.
Re-arming replaces a pending attempt; `WIFI_EVENT_STA_CONNECTED` cancels it, and
the callback does nothing if the station is already associated (a failed connect
in `wifi_connect_pinned()` would drop the BSSID pin). PROVEN (code), NOT TESTED
on target.

## 3. Required scenarios (`GEMINI.md` §6.3) — status

| # | Scenario | Status |
|---|---|---|
| 1 | Normal startup DHCP + DNS | MEASURED on target (baseline: station mode, pool connected) |
| 2–3 | Delayed / temporarily absent DHCP | policy TESTED (`test_net_recovery_policy`: reassert does not fight a running client); NOT MEASURED |
| 4–5 | AP disappears, returns (also after > 1 retry window) | policy TESTED (never gives up); NOT MEASURED |
| 6 | Link back, no lease yet | policy TESTED; NOT MEASURED |
| 7 | Lease changes ip/gw/mask/dns | PARTIALLY PROVEN: lease capture validation TESTED (malformed/partial leases not persisted); an actual lease change end-to-end NOT TESTED |
| 8–10 | DNS-only failure, pool DNS failure, pool reconnect with Wi-Fi up | NOT TESTED in this release (pre-existing V1 retry/failover code, unchanged) |
| 11 | Fresh lease while fallback latched | TESTED (`a genuine lease clears the fallback latch`) |
| 12 | Previous lease without gateway / invalid mask | TESTED (`rung 1 requires a complete lease`) |
| 13 | 0.0.0.0, multicast, broadcast, malformed, non-contiguous mask | TESTED (`ipv4 validation`, `ipv4 netmask validation`) |
| 14 | Concurrent reconnect events, one winner | PROVEN (code: single esp_timer re-armed); NOT TESTED |
| 15 | DNS from real lease, not persisted local guess | TESTED (`lease resolver persistence`) |
| 16 | NVS absent/damaged → safe defaults, no static IP | PROVEN (defaults off/empty) |
| 17 | Long outage: no leak, no log flood, no reboot loop | PARTIALLY PROVEN: bounded delays TESTED; heap/log behaviour NOT MEASURED |
| 18 | Resume after prolonged outage without intervention | policy TESTED; NOT MEASURED |

No router was rebooted or reconfigured for any of these (forbidden by
`GEMINI.md` §6.3 without separate authorization).

## 4. Known open points

- `ip_timeout_callback` runs in the FreeRTOS timer service task (2048 B stack)
  and blocks it for 250–300 ms on the bounce / re-associate rungs, plus NVS reads.
  PLAUSIBLE risk to other software timers; no failure observed. Not changed.
- `NET_STATE_WIFI_AUTH_FAILED` is now a label for "slow retry", not a terminal
  state. The API's `wifiStatus` string still shows the last reason code.
