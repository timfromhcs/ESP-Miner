# Plan v3 - Netzwerk-Reliability, moderne Router, und ein ehrlicher Blick auf den "AI-Ranker"

Stand: 2026-10-04. Ziel: beide Einheiten kommen nach **jedem** Kaltstart in Sekunden
zuverlaessig online, und die Firmware ist die Sorte Code, die ein modernes Mesh
oder einen WPA3-Router nicht mehr ueberrascht.

Dieser Plan ersetzt die Diagnose in Abschnitt 11 von
`PLAN_V2_17_STALE_AND_LIMITS.md`, die sich als **falsch erwiesen hat**. Die
Korrektur steht in Abschnitt 1.

---

## 0. Was wir jetzt wirklich wissen

| Einheit | Zustand | Beleg |
|---|---|---|
| B `blackharkminer` `.61` (MAC `74:4D:BD:77:99:80`) | **miningt**, `v2.17.0-hardened`, ~408-494 GH/s, 20 akzeptiert / 0 rejected, `notifyDropped=0` | API + Pool-Shares |
| A `timsminer` `.66` (MAC `74:4D:BD:77:DD:3C`) | **offline**, laeuft noch `v2.16.0` | 5x `DHCP_TIMEOUT`, kein ARP-Eintrag |

Beide hängen am selben Mesh (`FRITZ!Box 7590 MP-2,4GhZ`, dazu Repeater
`Repeater Balkolaris` / `wifi repeater` und ein `FRITZ!Powerline 1260`).
**Beide bekommen aktuell keinen DHCP-Lease.** Der PC und der Router selbst
bekommen problemlos IPs - es betrifft also gezielt die 2,4-GHz-Clients der
Miner.

Das ist die eine Tatsache, auf die sich alles Weitere aufbauen muss: **auf
diesem Netz ist DHCP unzuverlaessig.** Jede Loesung, die DHCP voraussetzt, ist
damit keine Loesung.

---

## 1. Korrektur der frueheren Diagnose (wichtig)

### Was ich previously behauptet habe

> "GATEWAY_PROBE,result=UNREACHABLE,sent=0,received=0 - das Geraet legt null
> Pakete auf die Leitung ... das ist Access-Point-Seite, nicht Firmware."

### Warum das falsch war

`sent=0` beweist **nicht**, dass nichts gesendet wurde. In ESP-IDFs
`ping_sock.c` gilt:

```c
sent = sendto(ep->sock, ep->packet_hdr, ep->icmp_pkt_size, 0, ...);
if (sent != (ssize_t)ep->icmp_pkt_size) {
    getsockopt(ep->sock, SOL_SOCKET, SO_ERROR, &opt_val, &opt_len);
    ESP_LOGE(TAG, "send error=%d", opt_val);   // -> wir sahen "send error=0"
    ret = ESP_FAIL;
} else {
    ep->transmitted++;                          // nur hier zaehlt transmitted
}
```

lwIP implementiert `getsockopt(SO_ERROR)` nicht - es liefert immer `0`. Wir haben
`send error=0` gesehen, konnten das echte errno also **nie** auslesen. `sent=0`
heisst lediglich "`sendto` ist fehlgeschlagen", nicht "es wurde nichts
gesendet". Die damalige Schlussfolgerung Access-Point war nicht belegt.

Und das Gegenbeispiel existiert: B ist mit exakt derselben Firmware **nicht**
on-line-gegangen, sondern hat nach dem Rettungsbuild **in ~3 Sekunden** eine
Lease-Adresse bezogen und mined. Eine Access-Point-Sperre erklaert das nicht.

### Der echte Bug

Der entscheidende API-Vertrag, gegen den ich verstossen habe:

> `esp_netif_set_ip_info()`: *"DHCP client/server **must be stopped** before
> setting new IP information."* Rueckgabewert bei laufendem Client:
> `ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED`

Also ist `esp_netif_dhcpc_stop()` **Pflicht und korrekt** - nicht der Bug. Und
zusaetzlich:

> `esp_netif_set_old_ip_info()`: *"This function is called from the DHCP client
> (if enabled), before a new IP is set. **It is also called from the default
> handlers for the `SYSTEM_EVENT_STA_CONNECTED`**."*

> *IP lost timer*: *"If the interface is disconnected or down for too long, the
> IP lost timer will expire (after the configured interval) and set the old IP
> information to zero."*

Mein Code macht:

```
esp_netif_dhcpc_stop(sta);          // korrekt, Pflicht
esp_netif_set_ip_info(sta, &ip_info);  // korrekt
...
esp_netif_dhcpc_start(sta);        // <-- DAS IST DER BUG
```

Nach dem Binden einer statischen Adresse wird der DHCP-Client **wieder
gestartet**. Damit laeuft die DHCP-Zustandsmaschine erneut, und:

1. beim naechsten DHCP-Timeout setzt esp-netif die gehaltene IP-Info wieder
   auf `0.0.0.0`,
2. jeder `STA_CONNECTED`-Default-Handler ueberschreibt die "old IP"-Info,
3. der IP-lost-Timer setzt sie nach einer gewissen Zeit ebenfalls auf `0`.

Ergebnis: die Adresse sitzt ein paar Sekunden, dann fliegt sie wieder raus.
Genau das sehen wir - B kam hoch und war wieder weg, A kam hoch und war wieder weg.

Das korrekte Muster ist das von Espressif selbst und von ESPHome
(`dhcpc_stop()` -> `set_ip_info()` -> **DHCP gestoppt lassen**). Es wird nie
wieder gestartet.

**Konsequenz fuer die Architektur:** eine Fallback-Adresse muss von *uns*
neu gesetzt werden, wenn sie verschwindet - nicht von DHCP.

---

## 2. Fix-Design: die Leiter, die haelt

### 2.1 `dhcpc_start()` nach dem Binden entfernen

Die einzige Aenderung an der bestehenden Reihenfolge. Danach bleibt der DHCP-
Client nach einem erfolgreichen Fallback gestoppt.

### 2.2 Re-Assert-Watchdog statt DHCP-Restart

Ein billiger Timer (1 Hz, nur ein `esp_netif_get_ip_info()`-Aufruf, ~keine
Kosten) prueft periodisch, ob die erwartete Adresse noch gebunden ist:

- Ist die IP `0.0.0.0` oder weicht sie von der erwarteten ab, wird der komplette
  Bind-Pfad erneut ausgefuehrt (ohne DHCP-Client zu starten).
- Ist alles in Ordnung, passiert nichts.

Das macht die Adresse stabil und macht 2.1 gefahrlos. Damit ist die Anforderung
"immer wieder schnell gehen" erfuellt: die Einheit ist nach einem Kaltstart in
~3 s da und bleibt da.

### 2.3 Lease vollstaendig speichern, nicht halb

Aktuell speichern wir IP und (neu) DNS, aber die Netmaske ist im
Last-Lease-Pfad hart auf `255.255.255.0` gespickt (`connect.c:758`). Auf einem
Netz mit `/23` oder einem anderen Segment erzeugt das eine Adresse, die nicht
im Netz liegt. Lease-Speicher braucht `ip`, `netmask`, `gw`, `dns` - das sind
allesamt Felder, die der DHCP-Client uns ohnehin gibt.

### 2.4 Leiter: Reihenfolge und Zeitpunkt

`DHCP_LADDER_AFTER_RETRY` ist bereits `1` - die Leiter wird nach dem ersten
Timeout versucht, nicht nach allen fuenf. Das ist richtig und bleibt.

Neu ist nur, dass die Leiter **belastbare Daten** braucht. Bei einer Einheit, die
nie einen Lease hatte (unser Fall B, und spaeter jede frisch gekaufte Einheit)
ist rung 1 leer. Deshalb:

1. rung 1: echter zuletzt gesehener Lease (inkl. Maske + Resolver)
2. rung 2: explizit konfigurierter statischer Fallback aus NVS
3. rung 3: **nichts** - zurueck in die DHCP-Schleife, mit Backoff

Wichtig: rung 2 nur, wenn der Operator eine Adresse **reserviert hat**. Kein
Default-Wert im Code. Ein Default, der eine konkrete Adresse nennt, laesst jedes
Geraet, das aus dieser Firmware gebaut wird, genau diese Adresse behaupten - das
ist auf jedem anderen Netz ein Duplicate-IP-Konflikt.

### 2.5 Zustand sichtbar machen

`/api/system/info` und AxeOS bekommen die effektive Netzquelle
(`dhcp` / `last-lease` / `static-config` / `none`), die gebundene Maske und den
Ergebniscode der letzten Bindung. Ohne das ist die naechste Fehlersuche wieder
Raten.

### 2.6 Diagnostics behalten

`GATEWAY_PROBE` (ICMP) und `GATEWAY_TCP` (echtes `errno`) bleiben. Sie haben
ihren Zweck erfuellt: sie haben die Behauptung "das ist der Access Point"
widerlegt. `GATEWAY_TCP` ist die einzige Stelle, die ein echtes errno liefert.

---

## 3. Moderne Router: was "mehr Treiber" hier wirklich heisst

Es gibt auf dem ESP32-S3 keine "Treiber fuer Router". Es gibt ein paar
Konfigurationsstellen, an denen moderne Mesh-/WPA3-Router uns aussortieren
koennen. Jede ist konkret und pruefbar:

### 3.1 Versteckte SSID - ein echter Bug

`connect.c:1321` setzt:

```c
.scan_method = WIFI_FAST_SCAN,
```

`WIFI_FAST_SCAN` filtert **nur** nach der konfigurierten SSID und bricht den
Scan nach dem ersten Treffer ab. Eine versteckte SSID sendet jedoch einen leeren
SSID-Namen im Beacon. Mit `FAST_SCAN` findet die Station sie **nie** und
verbindet sich nie. Das ist keine Feinheit, das ist Totalausfall in Hotspots,
Cafés und sehr vielen Firmen-Netzen.

Fix: `WIFI_ALL_CHANNEL_SCAN` mit anschliessendem Filter auf die
konfigurierte SSID. Kostet etwas Scan-Zeit, macht aber jedes Netz erreichbar.

### 3.2 Mesh mit identischer SSID

`WIFI_CONNECT_AP_BY_SIGNAL` waehlt das staerkste BSSID. In einem Mesh kann das
der falsche Knoten sein - etwa einer, der ein getrenntes VLAN hat, keinen DHCP
Relay macht oder Client-Isolation faehrt. Das ist genau die Fehlerklasse, an der
beide Einheiten gerade leiden.

Fix: BSSID-Allowlist konfigurierbar machen. Wer weiss, welcher Knoten der
richtige ist, pinnt ihn; alle anderen scannen normal. Das ist die einzige
Massnahme, die 2.4-GHz-Mesh-Probleme zuverlaessig loest.

### 3.3 Laenderschema und Sendeleistung

Es ist kein Laenderschema gesetzt (`esp_wifi_set_country_code` fehlt), damit
arbeitet der Chip im World-Domain-Default: andere Kanalgrenzen, andere
Maximalleistung, schlechteres Roaming. In DE explizit setzen.

### 3.4 DHCP-Robustheit in lwIP

In `sdkconfig` steht:

```
CONFIG_LWIP_DHCP_DOES_ARP_CHECK=y          <- gut, RFC 3203
# CONFIG_LWIP_DHCP_DOES_ACD_CHECK is not set   <- fehlt
# CONFIG_LWIP_DHCP_RESTORE_LAST_IP is not set   <- fehlt
```

- **ACD** (Address Conflict Detection, RFC 3927) einschalten: die Station prueft
  eine angebotene Adresse per ARP, bevor sie sie uebernimmt. Das ist der
  Schutz, der unseren eigenen Fall verhindert haette, in dem ein Geraet eine
  fremde Adresse zieht.
- **`LWIP_DHCP_RESTORE_LAST_IP`** einschalten: lwIP stellt nach einem Reboot die
  zuletzt erhaltene IP sofort wieder her, statt auf den ersten Lease zu warten.
  In Verbindung mit 2.3 verkuerzt das die Kaltstartzeit weiter.

### 3.5 WPA3 / PMF / OWE

Kompilerseitig ist das bereits vollstaendig und richtig:

```
CONFIG_ESP_WIFI_ENABLE_WPA3_SAE=y
CONFIG_ESP_WIFI_ENABLE_SAE_H2E=y
CONFIG_ESP_WIFI_ENABLE_WPA3_OWE_STA=y
CONFIG_ESP_WIFI_ENABLE_WPA3_OWE_STA=y
CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT=y
CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK=y
```

Der STA laeuft auf PMF Optional, was WPA3-Personal verlangt. Was fehlt, ist eine
Laufzeit-Option `pmf required` fuer APs, die bei einem Konflikt lieber hart
trennen, und ein expliziter Hinweis in der Doku, dass WPA3-Personal
(`WIFI_AUTH_WPA3_PSK`) und WPA2/WPA3-Transition zwei verschiedene Modi sind -
Einheiten mit einer WPA2-Only-Config kommen sonst nicht rein.

### 3.6 Netz-Checks vor dem Pool-Connect

- mDNS abschaltbar machen: viele IoT-/Gastnetze blockieren Multicast, und
  `spawn_mdns_init_if_needed()` laeuft im kritischen Pfad.
- Reconnect-Grundcodes sauber auswerten (Tabelle existiert schon,
  `connect.c:1475`) und daraus Backoff ableiten statt blind neu zu verbinden.
- Einen Pfad fuer IPv6-only-Netzte? spaeter; die Resolver liefern aktuell `EAI_FAIL`.

---

## 4. Der "AI-Ranker" - was ehrlich machbar ist

Der Wunsch ist nachvollziehbar: ein Modell, das die Einheit "klueger" betreibt,
damit sie mehr mined. Ich habe dafuer im Netz nachgesehen. Die ehrliche
Antwort hat drei Teile.

### 4.1 ONNX Runtime: nein, nicht auf ESP32-S3

Es gibt keine ONNX-Runtime fuer ESP32-S3. Der Wunsch nach "KI" hat hier aber
trotzdem eine solide Grundlage, denn Espressif liefert zwei echte Inference-
Stacks: **esp-dl** (NN-Inferenz, 6103 µs fuer ein kleines Modell auf dem S3) und
**TensorFlow Lite Micro**. Beide brauchen allerdings int8-Quantisierung und
ein uebersetztes Modell - mitten in einer Firmware, die keinen Modell-Trainings-
oder Konvertierungsschritt hat, ist das eine echte Abhaengigkeit, keine
Zeilenverschiebung.

### 4.2 Ein Modell kann die Hashrate nicht erhoehen

Die Hashrate ist durch Silizium, Spannung und Frequenz gesetzt. Ein Classifier
aendert daran nichts. Was ein "Ranker" tatsaechlich tun kann, ist **eine
Entscheidung treffen, unter welchen Betriebspunkten die Einheit laeuft**:
Frequenz, Spannung, Temperaturziel, Job-Intervall, Batch-Groesse, ob ein Job
noch takeable ist.

Das ist eine Entscheidung ueber eine Handvoll Kandidaten. Dafuer ist ein
**deterministisch erklaerbarer Regel-Ranker** (geschuetzte Margen, explizite
Gewichte, auditierbar) die richtige Form - schneller, kleiner, ohne
Ueberraschungen und im Notfall abschaltbar.

### 4.3 Was wir dafuer zuerst brauchen: Daten

Wir haben keine Telemetrie, auf der sich ein Modell trainieren liesse. Ein
"Trainer" wuerde hier nichts lernen, sondern nichts. Deshalb ist die Reihenfolge
fest:

1. **Instrumentierung zuerst** - und die haben wir bereits: die
   Notify-Drop-Gruende, `force_clean_pending`, Job-Intervall-Clamp, Accept/Reject
   und Altersverteilung sind genau die Labels, die ein spaeteres Modell
   braucht.
2. **Regel-Ranker** ueber diese Signale, mit Gewichten, die man begruenden kann.
3. **Erst wenn** die 4-Stunden-Messung pro Einheit vorliegt: Evaluation, ob
   ueberhaupt Signal da ist. Ein Gradient-Boosted-Tree auf ~10 Merkmalen ist
   dann die ehrliche Wahl, kein Neuronales Netz.
4. **Optional** ein winziges int8-Modell ueber esp-dl, das *empfiehlt*, dessen
   Ausgabe die Regelschicht anschliessend klemmt. Kein Modell darf je direkt
   Frequenz oder Spannung setzen.

Erwartungsmanagement: **kein Hashrate-Gewinn ist zu erwarten.** Der Gewinn liegt
in weniger Stales, niedrigerer Temperatur, weniger Leistung und stabileren
Shares. Das ist weniger aufregend und deutlich wertvoller.

---

## 5. Reihenfolge

| # | Schritt | Fertig wenn |
|---|---|---|
| 1 | `dhcpc_start()` nach dem Binden entfernen | Code review |
| 2 | Re-Assert-Watchdog (2.2) | Build gruen |
| 3 | Lease speichert `ip`, `netmask`, `gw`, `dns` vollstaendig (2.3) | Host-Tests |
| 4 | `scan_method` auf `WIFI_ALL_CHANNEL_SCAN` (3.1) | Build gruen |
| 5 | `ACD` + `RESTORE_LAST_IP` in `sdkconfig` (3.4) | Build gruen |
| 6 | Laenderschema DE (3.3) | Build gruen |
| 7 | Netquelle in API ausgeben (2.5) | `GET /api/system/info` zeigt `dhcp`/`last-lease`/`static` |
| 8 | BSSID-Allowlist (3.2) | Build + AxeOS-Feld |
| 9 | Beide Einheiten flashen, je 3 Kaltstarts | Jede kommt < 10 s online und mined |
| 10 | `v3.0.0`, README, CHANGELOG, Release | Tag + Push |
| 11 | 4-h-Messung pro Einheit | `notifyDropped`, Stale-Rate, Accept-Rate protokolliert |
| 12 | Erst danach Regel-Ranker | Vergleichsmetrik definiert |

Schritte 1-3 sind der eigentliche Fix. Schritte 4-6 sind billig und
verhindern, dass wir denselben Fehler in einem anderen Netz noch einmal suchen.

## 6. Gate, das nicht verhandelbar ist

Kein Release, bevor jede Einheit **drei Kaltstarts in Folge** in unter 10
Sekunden online ist und danach minet. Ein einzelner guter Boot zaehlt nicht -
genau der hat uns vorhin glauben lassen, es sei in Ordnung.