# Post-flash snapshot: v2.16.0-hardened on 192.168.178.66

Captured after the v2.16.0 OTA and then a USB (COM3) re-flash, while the unit was
mining. No secrets: the device masks pool passwords as `*****`, and no extranonce
appears in the log.

Compare against the pre-change baseline in
[`../snapshots/20261004-003232/`](../snapshots/20261004-003232/) and the design
rationale in [`../../../docs/PLAN_BITAXE_BM1366_OPTIMIZATION.md`](../../../docs/PLAN_BITAXE_BM1366_OPTIMIZATION.md).

## Flash path

| Step | Detail |
|---|---|
| 1st flash | OTA over WLAN, `POST /api/system/OTA`, app image only → `ota_0` |
| 2nd–5th flash | USB Serial/JTAG `COM3` (`VID_303A&PID_1001`), `esptool write_flash 0x710000 esp-miner.bin` |
| NVS | **never** erased — preserved across every flash |
| Final version | `v2.16.0-hardened`, running from `ota_0` |

App-only images were used throughout, so the `nvs` and `phy_init` partitions were
never touched. `nvs_config: Used entries: 244` confirms the settings survived.

## Settings preserved (unchanged from before the upgrade)

| Field | Value |
|---|---|
| IP | `192.168.178.66` — real **DHCP lease**, not the static fallback |
| MAC | `74:4D:BD:77:DD:3C` (efuse factory MAC, override removed) |
| Hostname / mDNS | `timsminer` / `timsminer.local` |
| SSID | `FRITZ!Box 7590 MP-2,4GhZ` |
| Pool 0 | `sha256.eu.mine.zpool.ca:3333` (SV1) |
| Pool 1 | `sha256.us.mine.zpool.ca:3333` |
| ASIC | 485 MHz, 1200 mV |

## Verified fixes

**P0 — undecodable jobs are no longer mined.** Two occurred in the first ~70 s:

```
W (28196) stratum_v1_task: Dropping undecodable job c17d (dropped=1 received=5), keeping ASIC on current work
W (67248) stratum_v1_task: Dropping undecodable job c1cf (dropped=2 received=8), keeping ASIC on current work
```

`keeping ASIC on current work` is the fix: previously such a job was enqueued
*first* and mined for a full job interval (`ASIC Job Interval: 2000 ms`), so
each one cost ~2 s of hashrate on work that could only ever yield stale shares.
In the baseline log this was job `72b7`, burned from `t=2349264596` to
`t=2349270625` (~6.7 s in that run).

**UART baud mismatch gone.** The log now shows host and chip agreeing:

```
I (57130) bm1366: Setting chip UART BT8D=2 -> 1041666 baud (host programmed to match)
I (57130) serial: Changing UART baud to 1041666
```

Previously the host was programmed to 1 000 000 while the chip ran at
25 MHz/24 = 1 041 667 — a 4.17 % mismatch.

**Ticket mask tracks pool difficulty:**

```
I (50102) bm1366: Setting ticket mask for pool difficulty 256.00
```

**DHCP fixed.** `IP_ACQUIRED, ip=192.168.178.66` after **2 966 ms** with the real
lease. Root cause was ordering: `esp_netif_dhcpc_stop()` leaves the netif down, and
`esp_netif_set_ip_info()` only pushes into the TCP/IP stack when the interface is
up — so the address never reached LwIP and the stack never answered ARP. Now
`dhcpc_stop → netif_set_up → set_ip_info`.

## Measured, 8 samples at 20 s (`telemetry-8x20s.csv`)

| Metric | Baseline v2.15.3 | v2.16.0 |
|---|---|---|
| Hashrate (1 h avg) | 433.0 TH/s | **429.7 – 430.2 TH/s** (steady) |
| Shares accepted | 88 425 | 69 → 80 over 140 s |
| Shares rejected | 222 (0.250 %) | **1** (1.25 % of 80, one stale race) |
| `notifyDropped` | n/a (not counted) | 2, both early, then flat |
| `notifyReceived` | n/a | 39 → 46 |
| Vcore | 1188 – 1239 mV (flapping) | **1192 mV** |
| Chip temp | 60 °C | 59 °C |
| Free PSRAM | 7 558 508 B | 7 571 680 B (no leak) |

The new split shows the single rejection was `sharesRejectedStale=1` (the
unavoidable new-job / in-flight-share race) with `sharesRejectedOther=0` — i.e.
**no real client defect**, which the old single `sharesRejected` number could not
distinguish.

## Known issues still open

* **Domain imbalance persists:** 79.5 / 120.3 / 141.7 / 111.6 TH/s. Recorded as
  P1-2; not addressed in this release (needs per-domain rotation correlation).
* **Intermittent DNS failures** appear in earlier boots of this log
  (`DNS resolution failed ... error: 202`) while the router was not answering.
  Mining recovered on its own; `stratum_v1_task` falls back to pool 1 and then
  reconnects. Worth watching if it recurs.
* `asicFastUart` and `autotuneVoltage` are both **0** (off) — deliberately, since
  neither has hardware validation yet.

## Reproducing

```powershell
$base = "http://192.168.178.66"
Invoke-WebRequest "$base/api/system/info"       -OutFile system-info.json
Invoke-WebRequest "$base/api/system/statistics" -OutFile statistics.json
Invoke-WebRequest "$base/api/system/scoreboard" -OutFile scoreboard.json
Invoke-WebRequest "$base/api/system/asic"       -OutFile asic.json
Invoke-WebRequest "$base/api/system/logs"       -OutFile bitaxe-logs.txt
```