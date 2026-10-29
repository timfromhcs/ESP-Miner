# Test Matrix

What each suite is, how to run it, and what kind of evidence it produces.
Results for a specific candidate live in `reports/<version>/TEST_RESULTS.md`.

| Suite | Kind | Command | Runs in CI | Covers |
|---|---|---|---|---|
| `simulation/test_notify_validate` | host unit | `cd simulation && make run` | `unittest.yml` host-tests | `mining.notify` field validation, hex helpers, captured anomalous job |
| `simulation/test_nbits_reference` | host differential vs independent reference | same | same | compact target (nBits) decoding, exhaustive exponent sweep, 20 000 random words |
| `simulation/test_power_policy` | host unit | same | same | ATM thermal policy, V/F tuner, power target clamps |
| `simulation/test_net_recovery_policy` | host unit | same | same | IPv4/netmask validation, lease capture, fallback ladder, reassert latch, Wi-Fi reconnect schedule |
| `simulation/test_ota_guard` | host unit + optional real-image vectors | same; `./test_ota_guard <app.bin> <factory.bin>` | same (synthetic only) | OTA size/header gate, factory/merged rejection |
| `simulation/test_virtual_board`, `test_dataflow_memory` | host simulation model | same | same | queue/epoch invariants of a **model**, not production code |
| all of the above under ASan + UBSan | host, sanitizers | `make clean && make CFLAGS="-Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -I." && make run` | same | memory errors, UB, leaks in the code they build |
| `test-ci` (component Unity tests) | target code in QEMU (esp32s3) | `tools/run_qemu_tests.sh` (needs `qemu-system-xtensa`, a checkout with symlinks) | `unittest.yml` build-and-test | ASIC packet/CRC/PLL, stratum parsing, mining, coinbase, base58/bech32 |
| AxeOS Karma/Jasmine | frontend unit | `cd main/http_server/axe-os && npm ci && npm run test:ci` | `unittest.yml` frontend-test | components/services against the generated API client |
| Firmware build | build | `idf.py build` (ESP-IDF v6.0.2) | `build.yml` | compiles, links, app fits 4 MiB slot |
| Physical target | hardware | `GEMINI.md` §12, §19 | no | only `192.168.178.80` (Board 601 / BM1370) |

Evidence kinds are not interchangeable: a host test proves the pure function, a
QEMU test proves target code without peripherals, the simulation suites prove a
model, and only the physical target produces `MEASURED` / `HARDWARE VALIDATED`.

## Local notes

- On Windows, `test-ci/CMakeLists.txt` is checked out as a text file because it
  is a symlink in Git (`core.symlinks=false`). Run QEMU tests from a Linux/WSL
  clone.
- `wsl.exe <cmd>` expands `$VAR` before `bash -c` sees it. Put multi-line shell
  in a script file.
- Karma on Windows can exit 1 with `ECONNRESET` after `TOTAL: N SUCCESS` while
  Chrome shuts down. Read the `TOTAL` line; CI (Linux) is authoritative.
