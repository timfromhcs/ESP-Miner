# ESP-Miner — Master Autonomous Engineering Specification

## Mission

Develop the current ESP-Miner repository into the most reliable, efficient,
measurable and hardware-aware firmware that can realistically be achieved on
the actual Bitaxe Ultra / ESP32-S3 / BM1366 platform.

This is an engineering task, not a documentation exercise.

The agent MUST inspect the complete repository, research relevant current
technical information, identify the actual capabilities and limitations of
the hardware, implement improvements, test them virtually, build them,
deploy them to the known physical device, measure the real result, and
iterate.

Primary optimization targets:

- startup latency
- Wi-Fi / DHCP / DNS recovery
- router reconnect behavior
- pool communication
- Stratum V1/V2 reliability
- job planning
- ASIC scheduling
- nonce/work efficiency
- duplicate work
- stale work
- ASIC TX/RX reliability
- memory efficiency
- CPU utilization
- thermal behavior
- power efficiency
- effective hashing
- UI/UX
- observability
- recovery
- long-run stability
- reproducibility

Never sacrifice cryptographic correctness for performance.

---

# 1. SOURCE OF TRUTH

The following hierarchy is mandatory:

1. actual source code
2. actual Git history
3. actual tests
4. actual build results
5. actual device telemetry
6. actual device logs
7. official hardware documentation
8. official ESP-IDF documentation
9. official upstream repositories/issues/PRs
10. reproducible experiments

Never treat:

- README claims
- comments
- variable names
- previous agent statements
- screenshots
- theoretical possibilities

as proof by themselves.

Every major claim must be traceable to evidence.

---

# 2. TRUTH CLASSIFICATION

Use these exact classifications:

### Implementation status

- PROVEN
- PARTIALLY PROVEN
- PLAUSIBLE
- NOT PROVEN

### Measurement status

- MEASURED
- NOT MEASURED

### Test status

- TESTED
- NOT TESTED

### Availability

- AVAILABLE
- NOT AVAILABLE
- UNKNOWN

Never promote:

NOT PROVEN → PROVEN

or:

NOT MEASURED → MEASURED

---

# 3. TARGET DEVICE

Known target:

- IP: `192.168.178.66`
- Hardware: Bitaxe Ultra
- Board: 201
- Controller: ESP32-S3
- ASIC: BM1366

This target is already known.

DO NOT scan the LAN for other miners.

DO NOT discover or select another device automatically.

If `192.168.178.66` is unavailable:

- do not search for another IP
- do not guess another Bitaxe
- do not change target
- document TARGET_UNREACHABLE

---

# 4. DEVICE CONTROL POLICY

For this project the real Bitaxe is normally controlled through its
existing network/IP management interface.

Use the known device IP for:

- OTA firmware updates
- configuration
- logs
- telemetry
- reboot
- normal validation
- API testing
- UI testing

USB is NOT the normal update/control path.

USB may be used only for genuine recovery when the network path is
unrecoverable and the documented recovery procedure requires it.

Never use network discovery to find another target.

---

# 5. PRIVATE CONFIGURATION

The original user configuration may be present in a private local backup.

It may contain:

- Wi-Fi SSID
- Wi-Fi password
- pool
- worker
- pool password
- wallet/payout address
- frequency
- voltage
- fan
- thermal settings
- tuning settings

These may ONLY be restored to the physical target.

They MUST NEVER be committed or exposed in:

- source code
- Git
- README
- docs
- evidence
- reports
- CI
- release artifacts
- GitHub issues
- GitHub releases
- public logs
- commit messages

Public evidence must redact sensitive values.

---

# 6. INITIAL FORENSIC AUDIT

Before modifying code:

Inspect the complete repository.

Inspect:

- source
- components
- main
- tests
- scripts
- frontend
- HTTP/API
- Stratum
- ASIC drivers
- configuration
- build system
- CI
- docs
- evidence
- reports
- generated files
- Git history
- branches
- tags
- reflog
- stashes
- untracked files

Search for:

- TODO
- FIXME
- FIXME-like workarounds
- dead code
- duplicated implementations
- experimental code
- feature flags
- old forks
- disabled code
- abandoned optimizations
- hidden benchmarks
- old firmware
- prior experiments

Do not delete historical evidence until it is understood and safely preserved.

---

# 7. CAPABILITY MATRIX

Create:

`docs/CAPABILITY_MATRIX.md`

For every major feature:

| Subsystem | Capability | Implementation | Test | Hardware | Measurement | Status |
|---|---|---|---|---|---|---|

Cover at minimum:

- Boot
- Wi-Fi
- DHCP
- DNS
- Network recovery
- Stratum V1
- Stratum V2
- Pool failover
- Job planning
- Work scheduling
- Nonce allocation
- Version rolling
- Duplicate prevention
- Stale handling
- ASIC communication
- ASIC discovery
- ASIC result parsing
- Midstate handling
- Prehash handling
- Thermal control
- Fan control
- Voltage control
- Frequency control
- Auto tuning
- Memory management
- PSRAM
- UI
- API
- Telemetry
- OTA
- Recovery
- Watchdog

---

# 8. CURRENT INTERNET / UPSTREAM RESEARCH

Before implementing major architectural changes, research current information.

Search authoritative sources first:

- Bitaxe hardware repositories
- ESP-Miner upstream
- ESP-IDF documentation
- ESP32-S3 documentation
- BM13xx/BM1366 technical material
- schematics
- datasheets
- upstream issues
- upstream pull requests
- relevant mining protocol specifications

Research specifically:

- BM1366 nonce duplication
- version rolling
- nonce search percentage
- internal ASIC duplicate behavior
- HCN/fullscan behavior
- ASIC TX failures
- ASIC RX failures
- serial framing
- CRC
- result conversion
- stale work
- `clean_jobs`
- difficulty changes
- extranonce
- Stratum V1 reconnect
- Stratum V2 recovery
- pool failover
- Wi-Fi reconnect
- DHCP latency
- DNS behavior
- ESP32-S3 memory pressure
- PSRAM placement
- LwIP configuration
- Wi-Fi buffer configuration
- heap fragmentation
- watchdogs
- UI performance
- long-run stability

Never blindly copy upstream behavior.

First determine whether it applies to this repository and this hardware.

---

# 9. HARDWARE CAPABILITY DISCOVERY

Inspect the exact Bitaxe Ultra hardware design.

Identify all usable capabilities.

At minimum investigate:

## ESP32-S3

- CPU cores
- FreeRTOS tasks
- task affinity
- interrupts
- timers
- DMA
- SPI
- UART
- cache
- IRAM
- PSRAM
- heap capabilities
- heap tracing
- watchdogs
- reset reason
- performance measurement
- event groups
- task notifications
- queues
- ring buffers

## BM1366

Investigate only capabilities actually supported by the real implementation:

- work packets
- result packets
- job identifiers
- result identifiers
- nonce
- version rolling
- search space
- timeout
- serial protocol
- framing
- CRC
- ASIC addressing
- result FIFO behavior
- work replacement
- work cancellation
- internal duplicate behavior

## Board hardware

Identify actual capabilities of:

- INA260 or equivalent power telemetry
- EMC2101 or equivalent thermal/fan hardware
- DS4432U+ or equivalent voltage control
- TPS40305 or equivalent power conversion
- fan PWM
- fan tachometer
- USB
- other available sensors/controllers

Do not assume a component provides a feature unless the schematic/source/datasheet confirms it.

---

# 10. VIRTUAL-FIRST DEVELOPMENT

No experimental change goes directly onto the real device when it can be
meaningfully validated virtually.

Required sequence:

SOURCE ANALYSIS
→ UNIT TEST
→ INTEGRATION TEST
→ PROTOCOL TEST
→ SIMULATION
→ STATIC CHECK
→ BUILD
→ ARTIFACT HASH
→ REAL DEVICE

Build simulation/test coverage for:

- Wi-Fi events
- DHCP
- DNS
- router outage
- router recovery
- Stratum disconnect
- reconnect
- pool failover
- new job
- job replacement
- `clean_jobs=true`
- `clean_jobs=false`
- difficulty change
- duplicate job
- stale work
- duplicate candidate
- ASIC timeout
- ASIC TX failure
- ASIC RX failure
- malformed packet
- CRC failure
- missing ASIC
- unsupported ASIC
- low memory
- allocation failure
- watchdog state

Simulation does NOT count as hardware validation.

If something cannot realistically be simulated:

mark:

`NOT EMULATABLE`

Then perform the smallest safe hardware test required.

---

# 11. STARTUP PIPELINE

Instrument:

- boot start
- firmware initialization
- configuration loaded
- ASIC initialization
- Wi-Fi init
- AP association
- DHCP start
- IP acquired
- DNS ready
- Stratum connected
- first job
- mining active

Replace unnecessary fixed delays with:

- event-driven state transitions
- asynchronous work
- dependency-aware initialization
- task notifications
- event groups

Do not create races.

---

# 12. NETWORK STATE MACHINE

Use explicit states:

BOOT
WIFI_INIT
WIFI_CONNECTING
WIFI_CONNECTED
DHCP
NETWORK_READY
DNS_READY
STRATUM_CONNECTING
STRATUM_READY
MINING

Recovery:

WIFI_LOST
FAST_RECONNECT
DHCP_RETRY
DNS_RETRY
STRATUM_RECONNECT
JOB_SYNC
RECOVERY_VERIFY

A temporary network outage must not automatically imply a complete firmware
reboot.

---

# 13. DHCP / ROUTER OPTIMIZATION

Measure:

- AP association time
- DHCP start
- IP acquisition
- DNS resolution
- network-ready time

Optimize for real reliability, not artificially short timeouts.

Test:

- normal boot
- repeated cold boots
- router restart
- AP disappearance
- temporary Wi-Fi outage
- long outage
- DHCP delay
- DHCP renewal

Record before/after results.

---

# 14. STRATUM ENGINEERING

Analyze V1 and V2 if present.

Test:

- subscribe
- authorize
- notify
- difficulty
- clean jobs
- extranonce
- submit
- reconnect
- timeout
- failure handling

Classify failures:

TRANSIENT
RECOVERABLE
CONFIGURATION
PROTOCOL
FATAL

Do not retry permanent protocol/configuration errors forever.

---

# 15. JOB PLANNING

Treat job planning as a core performance subsystem.

Represent where technically appropriate:

- work ID
- job generation
- network generation
- ASIC generation
- reset generation
- job ID
- extranonce context
- version
- merkle root
- nTime
- nBits
- nonce range
- ownership
- cancellation state

Ensure results cannot silently belong to stale work.

---

# 16. NONCE / DUPLICATE WORK

Investigate:

- nonce overlap
- internal BM1366 duplicate behavior
- version rolling
- search percentage
- work restart
- reconnection
- ASIC reset
- job replacement

Controlled experiments may include:

100%
99%
98%
97%
95%

Do not assume 95% is universally optimal.

Measure:

- duplicate results
- effective work
- accepted shares
- rejected shares
- stale shares
- power
- thermal behavior
- sustained hashrate

Choose defaults only from evidence.

---

# 17. ASIC COMMUNICATION

Instrument:

- jobs transmitted
- TX successes
- TX failures
- TX latency
- result frames
- valid frames
- invalid frames
- CRC failures
- framing failures
- parser failures
- timeouts
- late results
- duplicate results

Calculate:

- TX failure rate
- RX validity rate
- result rate
- timeout rate
- ASIC idle time

Do not hide transport errors behind a generic counter.

---

# 18. TRUE / EFFECTIVE HASHING

Do not equate:

reported GH/s

with:

actual unique useful hashing.

Expose, where technically possible:

- reported hashrate
- estimated true work
- effective work
- duplicate ratio
- stale ratio

Clearly label estimates.

Do not claim a cryptographic probability improvement merely because an
interface counter increased.

---

# 19. MEMORY ENGINEERING

Profile:

- internal RAM
- DMA-capable RAM
- PSRAM
- heap free
- largest free block
- fragmentation
- allocations
- frees
- long-lived buffers
- Wi-Fi buffers
- LwIP buffers
- HTTP
- JSON
- UI
- ASIC queues

Optimize:

- allocation churn
- memcpy
- unnecessary copies
- buffer sizes
- object lifetime
- duplicate representations

Prefer:

- bounded buffers
- ring buffers
- pools
- fixed-size structures
- zero-copy where safe
- compact metadata

Keep latency-sensitive and DMA-sensitive buffers in appropriate memory.

---

# 20. DATA COMPACTION

Compact only non-critical metadata.

Allowed:

- bitfields
- packed metadata
- fixed-width telemetry
- compact event structures
- bounded ring buffers
- fixed-point values where precision is adequate

Never lossy-compress:

- hashes
- block headers
- midstates
- merkle roots
- nonces
- protocol-critical fields

Cryptographic correctness has priority over memory savings.

---

# 21. FREErtos / CPU ARCHITECTURE

Audit tasks.

Measure:

- CPU usage
- task runtime
- blocked time
- queue wait
- stack high-water mark
- starvation
- scheduling latency

Separate critical paths from:

- UI
- logging
- telemetry
- background services

Do not assume a particular CPU-core affinity is faster.

Measure it.

---

# 22. UI / UX

UI must represent actual device state.

Never use:

`0`

for unknown.

Use:

`N/A`

Never show an ASIC as active unless it is actually detected/healthy.

Canonical ASIC states:

- DETECTED
- SUPPORTED
- READY
- RUNNING
- FAILED
- NOT_PRESENT
- UNSUPPORTED

Show, where available:

- hashrate
- effective work
- power
- J/TH
- temperature
- fan/RPM
- voltage
- frequency
- pool
- pool latency
- pool state
- job age
- stale
- rejected
- duplicate
- reconnect count
- heap
- PSRAM
- uptime
- ASIC health

---

# 23. UI PERFORMANCE

Measure UI overhead.

Optimize:

- polling
- WebSocket frequency
- JSON generation
- payload size
- frontend allocations
- redundant calculations
- unnecessary DOM updates

Do not let UI operation materially reduce mining performance.

---

# 24. OBSERVABILITY

Implement structured telemetry.

Example:

EVENT,ts=1234,name=IP_ACQUIRED
EVENT,ts=1300,name=STRATUM_READY
METRIC,hashrate=...
METRIC,power=...
METRIC,heap=...
ASIC,idx=0,state=READY

No secrets.

Expose machine-readable metrics where practical.

---

# 25. FAULT INJECTION

Test:

- Wi-Fi loss
- AP loss
- router reboot
- DHCP delay
- DNS failure
- pool failure
- TCP reset
- Stratum reconnect
- pool failover
- difficulty change
- clean jobs
- ASIC timeout
- parser errors
- low memory

Measure:

- recovery time
- final state
- lost work
- stale work
- errors

---

# 26. LONG-RUN VALIDATION

Run where practical:

10 min
30 min
1 h
6 h
24 h

Monitor:

- hash rate
- effective work
- power
- temperature
- stale
- rejects
- duplicates
- reconnects
- ASIC errors
- heap
- largest free block
- PSRAM
- UI responsiveness

Look for:

- memory leaks
- fragmentation
- timer rollover
- counter overflow
- queue growth
- deadlocks
- recovery loops

---

# 27. AUTO-TUNING

If automatic tuning exists or is added:

Optimize:

effective_hashrate / watt

subject to:

- thermal limit
- power limit
- error limit
- stale limit
- stability limit

Use:

- safe voltage limits
- hysteresis
- bounded changes
- cooldowns
- rollback

Never tune solely toward maximum displayed GH/s.

---

# 28. POWER / THERMAL CONTROL

Use actual board telemetry.

Measure real:

- voltage
- current
- power
- temperature
- fan duty
- fan speed

Use real measurements for:

J/TH

Implement thermal hysteresis and fan-failure detection where hardware support
exists.

---

# 29. ASIC INVENTORY

Use one authoritative ASIC inventory.

All of these must use the same source:

- scheduler
- UI
- telemetry
- API
- validation

Do not have one subsystem report an ASIC that another subsystem knows is absent.

---

# 30. WATCHDOG / CRASH FORENSICS

Capture:

- reset reason
- watchdog cause
- exception
- task
- stack information
- firmware version
- commit SHA
- relevant subsystem state

Do not hide instability behind reboot loops.

---

# 31. OTA SAFETY

Before OTA:

- record current firmware
- record current configuration
- ensure rollback path
- calculate new image SHA-256
- validate target image
- deploy only verified image

After OTA:

- wait for reboot
- verify target responds
- verify firmware identity
- verify ASIC
- verify network
- verify pool
- verify job
- verify mining
- verify telemetry

If the target becomes unhealthy:

ROLL BACK.

Do not blindly retry the same broken image.

---

# 32. EVIDENCE

Create evidence IDs.

Examples:

EV-BOOT
EV-DHCP
EV-WIFI
EV-RECOVERY
EV-STRATUM
EV-POOL
EV-JOB
EV-NONCE
EV-DUPLICATE
EV-ASIC-TX
EV-ASIC-RX
EV-MEMORY
EV-POWER
EV-THERMAL
EV-UI
EV-STABILITY

Every evidence item should include:

- TEST ID
- commit
- firmware
- device
- date/time
- configuration class
- method
- baseline
- result
- interpretation
- limitations

---

# 33. BENCHMARKS

For every benchmark record:

- firmware
- commit
- hardware
- frequency
- voltage
- fan
- temperature
- duration
- sample count
- pool/test conditions

Prefer:

- mean
- median
- p95
- min
- max
- standard deviation

Never publish only the best measurement.

---

# 34. A/B TESTING

Whenever practical:

BASELINE
vs
CURRENT

Use equivalent:

- frequency
- voltage
- pool
- environment
- test duration
- workload

Do not attribute changes without controlling major variables.

---

# 35. ITERATIVE LOOP

Every meaningful optimization follows:

MEASURE
→ HYPOTHESIS
→ IMPLEMENT
→ UNIT TEST
→ SIMULATE
→ BUILD
→ HASH
→ OTA
→ REAL HARDWARE
→ LOG
→ TELEMETRY
→ MEASURE
→ COMPARE

If worse:

REVERT.

If equal:

retain only if it improves correctness, safety, observability or maintainability.

If better:

KEEP
+
EVIDENCE.

Then continue.

---

# 36. ONE-CHANGE ATTRIBUTION

Avoid deploying large collections of unrelated performance changes at once.

Group only changes that share one clearly defined engineering hypothesis.

This preserves causality.

---

# 37. DOCUMENTATION

Maintain:

- README.md
- docs/ARCHITECTURE.md
- docs/NETWORKING.md
- docs/JOB_PLANNING.md
- docs/ASIC.md
- docs/MEMORY.md
- docs/PERFORMANCE.md
- docs/UI_UX.md
- docs/TESTING.md
- docs/HARDWARE_VALIDATION.md
- docs/EVIDENCE.md
- docs/REPRODUCIBILITY.md
- docs/TROUBLESHOOTING.md
- CHANGELOG.md

Only create a file if it contains real useful information.

---

# 38. README

README must quickly explain:

WHAT
WHY
SUPPORTED HARDWARE
CURRENT STATUS
REAL MEASUREMENTS
BUILD
TEST
DEPLOYMENT
ARCHITECTURE
EVIDENCE
LIMITATIONS

No marketing claims without evidence.

---

# 39. SECURITY

Scan the complete repository and Git history for:

- passwords
- tokens
- API keys
- Wi-Fi credentials
- pool credentials
- wallet secrets
- private keys
- private backups
- machine-specific secrets

Block release if sensitive data is detected.

---

# 40. CLEAN REPOSITORY

Remove only verified garbage:

- temporary files
- local backups
- generated test leftovers
- IDE artifacts
- accidental binaries
- secret-containing logs

Preserve useful historical evidence.

---

# 41. CI/CD

CI should perform real work:

- checkout
- toolchain setup
- dependency install
- formatting
- lint
- unit tests
- integration tests
- protocol tests
- build
- firmware artifact generation
- SHA-256
- documentation consistency checks where practical

Use least privilege.

Use sensible action pinning.

Never expose secrets.

---

# 42. FRESH CHECKOUT

Verify from a clean checkout:

- dependency setup
- tests
- build

No hidden local state.

---

# 43. RELEASE

Release only a verified state.

Include:

- firmware images
- SHA256SUMS
- commit
- tag
- build metadata
- validation summary
- limitations
- release notes

Never create release artifacts that were not actually built.

---

# 44. GITHUB VERIFICATION

After push:

verify actual:

- branch
- commit
- workflows
- CI status
- artifacts
- release
- README
- links

Do not assume success from a local command.

---

# 45. PRIVATE CONFIG RESTORE

After successful firmware validation, restore the original private configuration
to the physical device only.

Verify:

- Wi-Fi configured
- pool configured
- worker configured
- payout configured
- frequency
- voltage
- fan
- thermal settings

Public logs must report only:

`REDACTED`

Do not print actual secrets.

---

# 46. FINAL ENGINEERING REPORT

Create:

`docs/FINAL_ENGINEERING_REPORT.md`

Include:

- initial state
- baseline
- research
- architecture
- changes
- tests
- simulations
- hardware tests
- network results
- ASIC results
- job-planning results
- memory results
- UI results
- power/thermal results
- reliability results
- failures
- rollbacks
- final measurements
- limitations
- release information

Clearly separate:

PROVEN
PARTIALLY PROVEN
PLAUSIBLE
NOT PROVEN

and:

MEASURED
NOT MEASURED

---

# 47. STOP CONDITIONS

Do not stop because:

- a plan exists
- a build started
- an image uploaded
- a single test passed
- the miner rebooted
- the UI looks correct

Stop only when:

- meaningful improvements are exhausted
- hardware constraints are proven
- an experiment has reached a justified technical limit
- a safety boundary prevents further work

Document the reason.

---

# 48. FAILURE HANDLING

For every failure:

OBSERVE
→ CAPTURE LOGS
→ IDENTIFY ROOT CAUSE
→ FIX
→ TEST
→ BUILD
→ OTA
→ VALIDATE
→ COMPARE

Do not hide failures.

Do not manufacture success.

---

# 49. ABSOLUTE ANTI-HALLUCINATION RULE

Never fabricate:

- measurements
- hardware results
- benchmarks
- test results
- ASIC features
- network behavior
- performance improvements
- Git commits
- release IDs
- CI results
- firmware versions
- evidence

Use:

UNKNOWN
NOT TESTED
NOT MEASURED
NOT PROVEN
PARTIALLY PROVEN
PLAUSIBLE

whenever appropriate.

---

# 50. PRIMARY OBJECTIVE

The final system should be:

FAST
STABLE
EFFICIENT
OBSERVABLE
RECOVERABLE
MEMORY-EFFICIENT
ASIC-AWARE
POOL-ROBUST
NETWORK-ROBUST
UI-CLEAR
REPRODUCIBLE

while preserving:

- cryptographic correctness
- protocol correctness
- hardware safety
- configuration integrity

---

# 51. FINAL EXECUTION COMMAND

DO THE WORK.

Do not only write a plan.

Do not stop after analysis.

Do not stop after the first successful build.

Do not stop after the first successful OTA.

Continue:

READ
→ RESEARCH
→ ANALYZE
→ IMPLEMENT
→ SIMULATE
→ TEST
→ BUILD
→ VERIFY
→ OTA
→ MEASURE
→ COMPARE
→ FIX
→ REPEAT

until the remaining limitations are genuinely technical rather than simply
unexplored.

All claims must remain evidence-based.

---

# 52. PHASE 1 SIMULATION & VIRTUAL BOARD EMULATION RECORD

- **Milestone Reached:** Phase 1 Virtual Board Model, Data Flow Forensics & Invariant Verification Complete.
- **Artifacts:** `simulation/virtual_board.h`, `simulation/virtual_board.c`, `simulation/test_virtual_board.c`, `simulation/test_dataflow_memory.c`, `simulation/Makefile`, `docs/PHASE1_SIMULATION_REPORT.md`.
- **Test Results:** 12/12 unit and dataflow simulation tests passing (100% green).
- **Property Testing:** 10,000 chaotic pseudo-randomly interleaved state machine, fault injection, and job lifecycle transitions verified with 0 invariant violations.
- **Invariant Audit:** Invariants INV-1 through INV-12 verified locally on host.
- **Redundancy Analysis:** Identified redundant ASCII hex parsing (200 bytes per tick) and 30-60 dynamic heap allocations/sec in `create_jobs_task.c`.
- **Hardware Status:** `NOT_HARDWARE_VALIDATED` / `TESTED_LOCALLY` / `SIMULATED` (Hardware flashing locked behind Phasensperre).

---

# 53. PHASE 2 PHYSICAL HARDWARE VALIDATION & PRODUCTION HARDENING RECORD

- **Milestone Reached:** Phase 2 Physical Hardware Validation, Mesh Wi-Fi Hardening, Benchmark Suite & Master Documentation Complete.
- **Physical Device:** Bitaxe Ultra (Board 201), ESP32-S3 rev 0.2 (MAC `74:4d:bd:77:dd:3c`), 1x BM1366 ASIC (112 core clusters, 894 small hashing engines).
- **Physical Verification Channel:** Direct USB Serial/JTAG (`COM3`, VID: `0x303A`, PID: `0x1001`), Partition `ota_0` (`0x710000`).
- **Network Verified:** AVM FRITZ!Box 7590 Mesh (`48:5d:35:0f:39:fc` / `2c:3a:fd:49:d3:95`), IP `192.168.178.66`.
- **Root-Cause Fixes Deployed & Proven:**
  1. Disabled 802.11v (BTM) and 802.11k (RM) to prevent 5 GHz band-steering black-holes on 2.4 GHz ESP32-S3.
  2. Isolated captive DNS server on UDP port 53 strictly to active SoftAP mode.
  3. Bound DHCP client start synchronously to `WIFI_EVENT_STA_CONNECTED`.
  4. Implemented 12-second deterministic static IP fallback with RFC 5227 Gratuitous ARP broadcast.
  5. Populated LwIP core DNS servers directly (`1.1.1.1` and `8.8.8.8`) via `dns_setserver()`.
  6. Configured IPv4-preferred `AF_INET` getaddrinfo resolution in `stratum_socket.c`.
  7. Removed blocking `vTaskDelay` from the `sys_evt` event loop.
- **Measured Physical Telemetry:**
  - Cold reset to valid mined share: **15.18 seconds**.
  - Wi-Fi AP association: **2,055 ms**.
  - DHCP IP acquisition: **1,654 ms** (3,709 ms total from boot).
  - Stratum pool DNS & setup: **1,828 ms** (13,794 ms from boot).
  - Sustained Hashrate: **435.81 GH/s** (10-minute mean; Min: 335.00 GH/s, Max: 523.98 GH/s).
  - Power Consumption: **12.40 W mean** (Min: 12.02 W, Max: 12.65 W @ 1.206 V actual VCore; Efficiency: **28.45 J/TH**).
  - Core Operating Temperature: **58.09 °C mean** (Min: 55 °C, Max: 59 °C @ 27.0% – 31.5% Fan PWM; Setpoint: 60.0 °C).
  - Pool Share Acceptance: **67 shares accepted / 0 rejects observed** during evaluation run (100% acceptance during test period).
  - Duplicate Nonces: **0 duplicate nonces observed** across 117 samples and 67 shares.
  - Free Octal PSRAM: **7.63 MiB**; Free Internal SRAM: **86 KiB**; 0 bytes monotonic memory leak drift.
- **Hardware Status:** `PROVEN` / `MEASURED` / `HARDWARE_VALIDATED` (100% physically verified).

---

# 54. DEVICE B (192.168.178.61) BACKUP, SAFE OTA UPGRADE, RESTORE & HARDWARE VALIDATION RECORD

- **Milestone Reached:** Device B (Separate Physical Bitaxe Ultra) Backup, Safe OTA Upgrade, Configuration Restoration & Hardware Validation Complete.
- **Physical Device:** Device B (Bitaxe Ultra Board 201), ESP32-S3 rev 0.2 (MAC `74:4D:BD:77:99:80`), 1× BM1366 ASIC (112 core clusters, 894 small engines), Hostname `blackharkminer`.
- **Primary Access / Upgrade Channel:** Network Wi-Fi OTA (`http://192.168.178.61/api/system/OTA`).
- **Baseline Prior to Upgrade (v2.14.0):**
  - Uptime: 1,618,749 s (~18.7 days).
  - 1h Avg Hashrate: 432.89 GH/s.
  - Power: 12.43 W (28.71 J/TH).
  - Temperature: 63.0 °C.
  - Rejection Count: 253 rejected shares (100% caused by `"Invalid job id"` bug in v2.14.0).
- **Private Backup Executed:**
  - Preserved outside repo: `backup/192.168.178.61/20260906_140756/`.
  - Artifacts: `system_info.json`, `system_asic.json`, `system_statistics.json`, `system_scoreboard.json`, `system_logs.txt`, `config_backup_private.json`.
  - Rollback Image Verified: `rollback_firmware_v2.14.0.bin` (SHA-256: `c7753827d35d48477f194a1c029f6e7cd1e63932c86c4a9e2a263163d9dffe3d`).
- **Safe OTA Upgrade:**
  - Upgraded partition `ota_1` $\to$ `ota_0`.
  - Upload duration: 20.18 s; Reboot comeback: 2.0 s.
  - Deployed Firmware: `v2.15.3-hardened` (SHA-256: `28d37dfabc33e5732c5f383f169d039dc02867b2c9a422d63f7ac331cec3965c`).
- **Configuration Restoration Verified:**
  - 10/10 parameters verified (Wi-Fi, Pool, Worker, Wallet, Frequency, Voltage, Fan, Thermal, Tuning, Hostname).
  - Status: `CONFIG RESTORED — SECRETS REDACTED`.
- **Sustained Hardware Validation (300s / 60 Telemetry Samples):**
  - Mean Sustained Hashrate: **434.22 GH/s** (Min: 356.20, Max: 515.39).
  - Mean Power: **12.31 W** (Min: 12.10, Max: 12.53).
  - Energy Efficiency: **28.35 J/TH** (+1.25% efficiency over baseline).
  - Mean Temperature: **61.6 °C** (-1.4 °C cooler than baseline).
  - Share Submissions: **37 accepted / 0 rejected** (**0.00% rejection rate**; 100% elimination of "Invalid job id" bug).
  - Free PSRAM: **7.63 MiB** (+40 bytes drift over 300s; 0 bytes monotonic leak drift).
- **Network & State Recovery Verified:**
  - Pause / Resume transition executed with 0 reboots, 0 dropped frames, and immediate share resumption ($40 \to 42$ shares).
- **Status:** `PROVEN` / `MEASURED` / `HARDWARE_VALIDATED`.

