# Evidence: Bitaxe Supra — 192.168.178.80

Diagnose- und Verifikationsunterlagen für die Bitaxe **Supra** (Board 601, BM1370).
Keine Secrets: das Gerät maskiert Pool- und WLAN-Passwörter selbst als `*****`,
und `/api/system/info` enthält kein WLAN-Passwortfeld. Die Stratum-Benutzerkennung
ist eine öffentliche payout-Adresse im Blockheader.

## Gerät

| Feld | Wert |
|---|---|
| IP | 192.168.178.80 (reservierte DHCP-Lease + statischer Fallback, FRITZ!Box 7590) |
| Hostname / mDNS | `bitaxe-tim2` / `bitaxe-tim2.local` |
| MAC | 28:84:85:A6:1B:E0 |
| Board | **601**, 1× BM1370, 128 Cores, 2 040 Small Cores, 4 Hash-Domains |
| Regler / Kernspannung | TPS546, 750 MHz @ 1.195 V actual |
| Display | SSD1306 128×32 |
| Pool | `sha256.eu.mine.zpool.ca:3333` (SV1), Fallback `sha256.us.mine.zpool.ca:3333` |
| Max. Leistung | 40 W (famility `Gamma`, `max_power`) |

## Update-Pfad

Ausschließlich **Wi-Fi OTA** (`POST /api/system/OTA`). Kein USB-Flash, kein
Network-Setting wurde angefasst.

| | vorher | nachher |
|---|---|---|
| Firmware | `v4.0.0-stable-tim2` | `v4.0.1-stable-tim2` |
| Partition | `factory` | `ota_0` → `ota_1` |
| Adresse | 192.168.178.80 | 192.168.178.80 (unverändert) |
| SSID / Frequenz / Spannung | FRITZ!Box 7590 MP-2,4GhZ / 750 MHz / 1195 mV | identisch |
| Statischer Fallback | `enabled=1`, `192.168.178.80` | identisch |

Die Partition wanderte `factory` → `ota_0` → `ota_1`, weil beide OTA-Slots beim
Start dieses Geräts leer waren und erst die beiden OTA-Vorgänge gefüllt haben.

## Dateien

| Datei | Inhalt |
|---|---|
| `system-info-pre-ota.json` | `GET /api/system/info` unmittelbar vor dem ersten OTA |
| `telemetry-v4.0.0-pre-ota.csv` | 400 s Baseline, 79 Samples im 5-s-Abstand (unveränderte Firmware) |
| `telemetry-v4.0.0-ticketmask-fix.csv` | 400 s nach dem ersten OTA (Ticket-Mask + Baud + Zähler) |
| `telemetry-v4.0.1-sustained.csv` | Langzeitlauf auf dem finalen Stand |
| `telemetry-*.summary.txt` | Kennzahlenübersicht je Lauf |
| `system-info-v4.0.1.json` | `GET /api/system/info` auf dem finalen Stand |
| `logs-v4.0.1-clean.txt` | Gerätelog, ANSI-stripped |
| `firmware.json` | SHA-256 und Größe des geflashten Images |

Rohdaten stammen aus [`tools/collect_telemetry.ps1`](../../tools/collect_telemetry.ps1).
Raten werden aus der Differenz zwischen erster und letzter Probe gebildet, nicht
aus den Boot-Zählern — sonst hängt der Vergleich an einem Zähler, der per
Pool-Reconnect oder Neustart zurückgesetzt wird.

> **Achtung zur CSV-Formatierung:** `Export-Csv` schreibt mit der System-Locale.
> Auf einem deutschen Windows steht deshalb Dezimalkomma in den Feldern. Beim
> Nachrechnen in SkriptenComma durch Punkt ersetzen, sonst liest man
> `1514,76` als `151476`.

## Nachweis aus dem Gerätelog

Zeilen, die belegen, dass die neuen Pfade laufen (Boot des finalen Stands):

```
bm1370: Setting chip UART BT8D=2 -> 1041666 baud (host programmed to match)
serial: Changing UART baud to 1041666
bm1370: nonce space: requested=750.00 MHz applied=750.00 MHz
bm1370: Version mask 1FFFE000 -> 65535 rolls
stratum_v1_task: Configure result accepted, version mask: 1fffe000
create_jobs_task: Set chip version rolls 65535
bm1370: Setting ticket mask for pool difficulty 2192.00
asic: Capping ticket mask difficulty 65536.00 -> 4096
bm1370: Setting ticket mask for pool difficulty 4096.00
system: Syncing clock
```

Bemerkenswert ist, was **fehlt**: keine einzige `W`- oder `E`-Zeile. Vor dem
Fix war das Log mit einer `ESP_LOGI`-Zeile pro Nonce geflutet, sodass der
komplette Boot-Trace innerhalb von Sekunden überschrieben war — das Gerät sah in
seinem eigenen Log gesund aus, während es kaputt war. Erst nach dem Umstieg auf
`ESP_LOGD` ist dieser Trace überhaupt lesbar.

## Vorher / nachher

Siehe [`../README.md`](../../readme.md#measurements) für die Tabelle und die
Auswertung. Rohzahlen in den CSV-Dateien oben.