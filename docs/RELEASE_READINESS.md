# Release Readiness — v4.0.2

Current state: **RC** (`v4.0.2-rc1`). Gates per `GEMINI.md` §23. Evidence:
`reports/v4.0.2-rc1/`.

| Gate | Requirement | Status |
|---|---|---|
| A | Fresh-clone build with recorded toolchain | ✅ clean clone in WSL, ESP-IDF v6.0.2 (`TEST_RESULTS.md`) |
| A | Firmware + production frontend build | ✅ general and `tim2` |
| A | No unreviewed/unrelated changes | ✅ 7 commits on `release/v4.0.2`, each scoped; operator's `GEMINI.md` edit left uncommitted |
| A | Generated API in sync | ✅ generated client is git-ignored and regenerated on every build/test |
| B | Host tests, strict warnings | ✅ 7 suites |
| B | ASan / UBSan | ✅ (after fixing the simulation leak) |
| B | QEMU tests | ✅ 78 / 0 / 0 |
| B | Frontend tests | ⚠️ 60/60 locally; Karma exit 1 on Windows teardown; Linux CI pending |
| B | Static analysis | ❌ not run (no cppcheck/clang-tidy locally) |
| B | New defects have regression tests | ✅ reconnect policy, OTA gate, simulation leak; ⚠️ OTA handler integration and the event-loop timer change have no automated test |
| C | DHCP/router recovery policy tests | ✅ host; physical outage NOT MEASURED |
| C | DNS/pool failover tests | ⚠️ not extended in this RC |
| C | ASIC driver tests | ✅ QEMU suite (no ASIC code changed) |
| C | Power/thermal checks | ✅ `test_power_policy` (no code changed) |
| D | Board/ASIC mapping | ✅ `tim2` image for Board 601 / BM1370; factory matrix unchanged |
| D | App vs factory image | ✅ documented, enforced by the OTA gate |
| D | Partitions / size | ✅ 34 % free in 4 MiB slot |
| D | SHA-256 manifest for all release assets | ❌ release workflow does not yet publish `SHA256SUMS` |
| D | CI logs for the exact candidate SHA | ❌ branch not pushed |
| D | No secrets in Git or assets | ✅ scan clean; raw evidence under git-ignored `artifacts/private/` |
| E | Target identity verified | ✅ Board 601 / BM1370, MAC == `tim2` pin |
| E | Exactly one OTA | ✅ 2026-10-29 20:38, HTTP 200, returned in ~10 s |
| E | Same IP, expected version | ✅ `192.168.178.80`, `v4.0.2-rc1-tim2`, `ota_1` |
| E | Smoke + sustained observation | see `reports/v4.0.2-rc1/HARDWARE_VALIDATION.md` |
| E | Rollback proven if claimed | ✅ not claimed (disabled) |

## Blocking Stable

1. GitHub Actions green for the exact release commit (requires pushing the branch).
2. `SHA256SUMS` published with the release assets.
3. The new OTA handler has not yet run on hardware: the RC was uploaded through
   the previous firmware's handler. The next OTA (rc2 or Stable) exercises it.
4. Pool credentials logged at INFO by `stratum_api` (`mining.authorize` tx line),
   readable via `/api/system/logs` — fix before Stable.

## Follow-up (not blocking)

- Enable and bench-test app rollback on a USB-attached unit.
- Angular major upgrade for the remaining advisories.
- Static analysis in CI.
- Physical router-outage test, only with explicit authorization.
