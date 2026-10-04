# Evidence: Bitaxe Ultra 192.168.178.66

Diagnose-Snapshots des produktiven Miners. Keine Secrets — Pool-Passwörter und
Stratum-Passphrase sind vom Gerät bereits als `*****` maskiert.

## Gerät

| Feld | Wert |
|---|---|
| IP | 192.168.178.66 (reservierte DHCP-Lease, FRITZ!Box 7590) |
| Hostname / mDNS | `timsminer` / `timsminer.local` |
| MAC | 74:4D:BD:77:DD:3C |
| Board | 201, 1× BM1366, 894 Small Cores, 4 Hash-Domains |
| Firmware | `v2.15.3-hardened`, IDF v6.0.2, Partition `factory` |
| Partitionen | `factory` = v2.15.3-hardened (aktuell), `ota_0` = v2.15.3-hardened, `ota_1` = v2.15.2-hardened-3-g2e19cd8-dir |
| ASIC | BM1366, 485 MHz, 1200 mV nominal / ~1206 mV actual |
| Pool | `sha256.eu.mine.zpool.ca:3333` (SV1), Fallback `sha256.us.mine.zpool.ca:3333` |
| Coin | DigiByte SHA-256d, Auszahlung auf geteilte `dgb1q…`-Adresse |

## Snapshots

| Verzeichnis | Inhalt |
|---|---|
| `snapshots/20261004-003232/` | `GET /api/system/info`, `/api/system/statistics`, `/api/system/scoreboard`, `/api/system/asic`, `/api/system/logs` |
| `snapshots/20261004-003232/bitaxe-logs.raw-ansi.txt` | Rohes Log (512-KB-Ringpuffer, ANSI-farbcodes) |
| `snapshots/20261004-003232/bitaxe-logs.clean.txt` | dasselbe Log, ANSI-stripped, 4 909 Zeilen — die Arbeitskopie |
| `telemetry-short.csv` | 6 API-Samples im 5-s-Abstand (`/api/system/info`) |

Der Log-Ringpuffer fasst 512 KB und umfasst zum Abrufzeitpunkt ca. 41 Minuten
Betriebszeit (`t=2 348 958 789` … `2 351 474 779`).

## Reproduzieren

```powershell
$b = "snapshots/<timestamp>"
$base = "http://192.168.178.66"
Invoke-WebRequest "$base/api/system/info"       -OutFile "$b/system-info.json"
Invoke-WebRequest "$base/api/system/statistics" -OutFile "$b/statistics.json"
Invoke-WebRequest "$base/api/system/scoreboard" -OutFile "$b/scoreboard.json"
Invoke-WebRequest "$base/api/system/asic"       -OutFile "$b/asic.json"
Invoke-WebRequest "$base/api/system/logs"       -OutFile "$b/bitaxe-logs.txt"
```

`/api/system/logs` liefert den kompletten Ringpuffer als `text/plain`
(`Content-Disposition: bitaxe-logs.txt`).

## Kernaussagen dieses Snapshots

Vollständige Auswertung und Maßnahmenplan:
[`docs/PLAN_BITAXE_BM1366_OPTIMIZATION.md`](../../docs/PLAN_BITAXE_BM1366_OPTIMIZATION.md)

* 3× `E stratum_v1_task: Failed to process mining notification` — der Job
  wurde trotzdem enqueued und ~6 s auf dem ASIC verbrannt (Job `72b7`).
* 4× `W stratum_v1_task: message result rejected: Invalid job id`
  (Stratum-Fehler 21), kumuliert 222 von 88 425 Shares = **0,250 %**.
* 3× `httpd_sock_err: error in recv : 104` (ECONNRESET, WebSocket-Clients).
* Domain-Imbalance `131,0 / 100,9 / 96,6 / 113,8` TH/s → **36 % Spread**.
* Vcore flackert zwischen 1188 mV und 1239 mV bei Sollwert 1200 mV.
* PSRAM stabil (7 558 508 B frei, `minFreeHeap` unverändert) → kein Leak.