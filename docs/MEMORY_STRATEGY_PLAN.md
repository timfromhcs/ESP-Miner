# MEMORY_STRATEGY_PLAN

Status: Schritte 1-3 und 5 umgesetzt und verifiziert. Offen: Schritt 7 (Hardware).

## 1. BELEGTE Ursache (nicht mehr Hypothese)

### Fehlersymptom

Linker-Abbruch:

    esp-miner.elf section `.noinit' will not fit in region `dram0_0_seg'
    DRAM segment data does not fit.
    region `dram0_0_seg' overflowed by 363232 bytes

Segment-Layout (ESP32-S3):

    dram0_0_seg   0x3fc88000   0x53700  = 341 KB interner DRAM (Maximum)

Die Obergrenze bestaetigt ESP-IDF selbst in
`components/esp_system/port/soc/esp32s3/Kconfig.memory`:

    config ESP32S3_FIXED_STATIC_RAM_SIZE
        range 0 0x54700   # Equal to I_D_SRAM_SIZE in linkerscript

### Verursacher

`main/log_buffer.c:38-39`:

    static EXT_RAM_NOINIT_ATTR log_buffer_header_t s_header;
    static EXT_RAM_NOINIT_ATTR char s_buffer[LOG_BUFFER_SIZE];

Messung VOR dem Fix, aus `build/esp-miner.map`:

    .noinit.1   0x3fca6280   0x80000  esp-idf/main/libmain.a(log_buffer.c.obj)  = 512 KB
    .bss                                             0xdf30                                =  57 KB

`.bss` war unauffaellig. Der gesamte Overflow kam aus EINEM Objekt:
dem 512-KB-Log-Ringpuffer.

### Der Mechanismus

`EXT_RAM_NOINIT_ATTR` ist ein bedingtes Attribut. Aus
`components/esp_common/include/esp_attr.h:154-157`:

    #define EXT_RAM_NOINIT_ATTR _SECTION_ATTR_IMPL(".ext_ram_noinit", __COUNTER__)
    #else
    #define EXT_RAM_NOINIT_ATTR __NOINIT_ATTR

Und `docs/en/api-guides/memory-types.rst:47`:

    "If the CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY is not enabled,
     EXT_RAM_NOINIT_ATTR will behave just as __NOINIT_ATTR, it will make data
     to be placed into .noinit segment in internal RAM."

Der C-Code war also korrekt. Veraltet war die Kconfig. In der WSL-Buildumgebung
wiesen `sdkconfig` und `sdkconfig.defaults` auseinander:

    versionierte sdkconfig.defaults:   CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY=y
    WSL sdkconfig (verdriftet):        # ... is not set

`sdkconfig` ist nicht versioniert. Es entsteht aus `sdkconfig.defaults` und
driftet ueber lokalen(menuconfig)-Gebrauch. Deshalb war der Effekt lokal und
nicht im Repository sichtbar, und der Bisection zeigte denselben Fehler in
jedem Commit, weil `sdkconfig` die Commits ueberlebt.

### Wirkungskette

    NOINIT_SEG_EXTERNAL aus
      -> EXT_RAM_NOINIT_ATTR faellt still auf __NOINIT_ATTR zurueck
      -> 512-KB-Logpuffer im internen .noinit
      -> dram0_0_seg (341 KB) ueberfaellt, Link bricht ab
      -> sobald es linkt: zu wenig interne RAM-Reserve fuer Task-Stacks
      -> "stack overflow in task IDLE1" und Reboot-Loop

## 2. Umgesetzte Massnahmen

### Schritt 1: Messen statt raten  (erledigt)

`idf.py build` bis zum echten Linkerfehler, dann `esp-miner.map` ausgewertet.
Ergebnis: ein einzelnes Objekt verursacht 100 % des Ueberlaufs.

### Schritt 2: Ursache beheben  (erledigt)

`CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY=y` und
`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` in `sdkconfig`.
Der Logpuffer liegt wieder in PSRAM.

### Schritt 3: Right-Sizing  (erledigt)

`LOG_BUFFER_SIZE` von 512 KB auf 128 KB reduziert
(`main/http_server/websocket.h`). Begruendung: Der Puffer wird bei jedem
Kaltstart vollstaendig genullt und per `esp_cache_msync` in die PSRAM-Cache-
Lines geschrieben (`main/log_buffer.c:205-206`). Das war bei 512 KB messbare
Boot-Zeit fuer Speicher, der nie gelesen wird.

Das ist bewusst eine zweite Verteidigungslinie: der Test mit absichtlich
falscher Kconfig zeigte, dass der Build mit 128 KB **immer noch** linkt,
weil 128 KB in die 176 KB interne Reserve passen. Mit 512 KB war das nicht
moeglich.

### Build-Guard  (erledigt)

`main/CMakeLists.txt` bricht den Build ab, wenn eine der beiden Optionen
nicht `y` ist. Damit kann der stille Rueckfall nicht unentdeckt bleiben.
Verifiziert: mit absichtlich falscher Option erscheint die Fehlermeldung,
mit korrekter Option laeuft der Build durch.

### Schritt 5: Overload-Schutz  (erledigt)

Neu: `main/memory_guard.c` / `main/memory_guard.h`, registriert in
`main/main.c` mit Stack in PSRAM (`MALLOC_CAP_SPIRAM`).

    - Ueberwacht groessten freien internen Block, nicht den Gesamt-Freiheap.
      Grund: FreeRTOS alloziiert Task-Stacks als zusammenhaengenden Block.
      40 KB fragmentierter Freihalp ist fuer einen 4-KB-Stack so unbrauchbar
      wie ein leerer Heap. Genau daraus entstehen die IDLE1-Panics.
    - Schwellen: Warnung < 24 KB frei, kritisch < 16 KB groesster Block.
    - Hysterese: Neustart erst nach 3 aufeinanderfolgenden kritischen
      Messungen (15 s), damit einzelne Allokationen kein Fehlalarm sind.
    - Bei bestaetigter Ueberlast: Diagnose loggen und kontrolliert
      `esp_restart()` statt einem unerklaerlichen Panic.
    - Kosten: zwei `heap_caps`-Aufrufe alle 5 s. Im ASIC-Pfad nicht messbar.
    - `memory_guard_internal_is_critical(needed_bytes)` als Hilfsfunktion fuer
      Aufrufer, die vor grossen DMA-Allokationen pruefen wollen.

### Schritt 4: Bluetooth  (bewusst nicht geaendert)

`main/setup_ble.c` gibt Classic-BT bereits frei (`esp_bt_controller_mem_release`,
Zeile 671) und deinitialisiert NimBLE ueber `nimble_port_stop()` /
`nimble_port_deinit()` (Zeile 727 ff.). `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`
wurde bewusst **nicht** gesetzt: NimBLE wird fuer das Provisioning gebraucht,
ein Umbau des Allokators koennte das brechen, ohne RAM zu gewinnen.

## 3. Verifiziertes Ergebnis

| Kennzahl | vorher | nachher |
|---|---|---|
| `.noinit` intern | 524 KB | 0 B |
| interne DRAM-Reserve | -363 KB | **+176 KB** |
| Logpuffer | 512 KB intern | **128 KB PSRAM** |
| `esp-miner.bin` | (Link abbruch) | 0x29dea0, 35 % frei |

Messung nach dem Fix, aus `build/esp-miner.map`:

    .ext_ram_noinit.1   0x3c283c60   0x20000  log_buffer.c.obj   = 128 KB in PSRAM
    _bss_end                       0x3fcb0690
    Segmentende                    0x3fcdb700
    Reserve                        0x2b070 = 176 KB

## 4. Leistungsbetrachtung

Alle Massnahmen liegen ausserhalb des ASIC-Hashpfads (DMA und eigene Buffer):

| Massnahme | Hashrate | Begruendung |
|---|---|---|
| Logpuffer nach PSRAM | keine | reiner Datenpuffer, kein Rechenpfad |
| Logpuffer 512 KB -> 128 KB | keine | spart Kaltstart-Zeit, kein Rechenpfad |
| Overload-Guard | keine im Normalfall | greift nur bei echter Ueberlast |
| Build-Guard | keine | Build-Zeit |

Der Data-Cache (32 KB) wurde **nicht** angefasst. Die ESP-IDF-Doku nennt
`CONFIG_ESP32S3_DATA_CACHE_SIZE` als RAM-Hebel, aber die Reduktion ist der
einzige hier verbleibende Eingriff mit theoretischer Wirkung auf den
Cache-Miss-Pfad des ASIC-Zugriffs. Er ist erst zu evaluieren, wenn Messungen
einen echten Bedarf zeigen. Aktuell ist die Reserve mit 176 KB ausreichend.

## 5. Offen

- [ ] **Schritt 6** CI-Paritaet: `sdkconfig.defaults` ist dokumentiert und
      korrekt. `sdkconfig` bleibt bewusst unversioniert, der Build-Guard
      macht Drift sichtbar.
- [ ] **Schritt 7** Host-Tests (`idf.py build test`) und drei Kaltstarts auf
      `.61` (`timsminer`, COM3), danach Geraet B.
- [ ] Messung, ob `esp_psram_get_free_size` / interne Reserve im Normalbetrieb
  stabil bleibt, ueber längeres Mining.

Kein Release und kein Push nach `main`, solange Schritt 7 offen ist.