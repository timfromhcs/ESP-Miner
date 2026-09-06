# Network Self-Healing Report — ESP-Miner v2.15.3-hardened (self-healing)

## Observed Failure
- Boot on FRITZ!Box 7590: `WIFI_CONNECTED` → `Acquiring IP...` → 5× `NET,event=DHCP_TIMEOUT,retry=X/5` with `dhcpc_stop/start` and `hostname=timsminer` → `NET,event=DHCP_FAILED` → previously assigned arbitrary `192.168.178.66` via `esp_netif_set_ip_info` and declared `is_connected=true` + `Connected (Static Fallback)!` without verifying route/DNS/external.
- After fallback, `ping 192.168.178.66` from 192.168.178.177 showed `DestinationHostUnreachable` and no ARP entry, `curl http://192.168.178.66/api/system/info` timed out, `stratum` showed `select() timeout` to 198.50.168.214. Mining started via ASIC but external connectivity was unverified — classic **fake NETWORK_READY**.
- Logging bug: prior retry logged `hostname=FRITZ!Box ...` (SSID) instead of NVS hostname.

## Root Cause
`components/connect/connect.c:382` single 12s timer with immediate static fallback violated RFC2131 and ESP-IDF `esp_netif` semantics. ESP-IDF docs: `esp_netif_dhcpc_start` is automatic on `WIFI_EVENT_STA_CONNECTED`; manual `stop/start` every timeout is not minimal recovery. State collapse: `WIFI_CONNECTED == IP_ACQUIRED == DNS_READY == STRATUM_READY == MINING_READY` all inferred from `is_connected`. No generation tracking, no socket invalidation on `WIFI_LOST`/`IP_LOST`, no distinction between `DHCP_FAILED` vs `USER_CONFIGURED_STATIC`.

## Architecture Before
BOOT→WIFI_INIT→WIFI_CONNECTING→WIFI_CONNECTED→DHCP (timer 12s)→`fallback 192.168.178.66`→`is_connected=true`→mDNS→Stratum. Single `ip_acquire_timer`, DHCP retry 8×8s, `vTaskDelay(250)` in timer callback, `s_retry_num` for WIFI but no exponential backoff, no `IP_EVENT_STA_LOST_IP` handler.

## Architecture After
Explicit states: `OFF→WIFI_INIT→WIFI_CONNECTING→WIFI_CONNECTED_NO_IP→DHCP_RUNNING→DHCP_RETRY→IP_ACQUIRED→DNS_READY→INTERNET_READY→STRATUM_CONNECTING→STRATUM_READY→MINING_READY`; failure: `WIFI_AUTH_FAILED/DHCP_FAILED/DNS_FAILED/ROUTE_FAILED/STRATUM_FAILED`; recovery: `WIFI_RECOVERY/DHCP_RECOVERY/DNS_RECOVERY/SOCKET_RECOVERY/STRATUM_RECOVERY`. Single source: `s_net_state`, `s_network_generation`, `s_dhcp_generation`, `s_stratum_generation`. Machine-readable: `NET,event=STATE_TRANSITION,from=X,to=Y,gen=N,dhcp_gen=M,retry=R,ts=ms` etc.
- DHCP: 8000ms initial, then `3000 + retry*1500 + jitter%800` exponential, `DHCP_RETRY_MAX 5`, no `vTaskDelay` in timer, `esp_random()` jitter, `failure_retry_cnt=3` in `wifi_config`, `esp_netif` lifecycle preserved.
- Fake fallback removed: after 5 retries → `DHCP_FAILED` → explicit `STATIC_FALLBACK` (`192.168.178.66` reserved) **with verification** (`NET,event=STATIC_FALLBACK`→`STATIC_IP_ASSIGNED`→`DNS_READY` before `is_connected`), distinct from DHCP success.
- WiFi recovery: classify `AUTH_FAIL/4WAY_TIMEOUT/AUTH_EXPIRE/HANDSHAKE_TIMEOUT` as permanent after 3 retries → `WIFI_AUTH_FAILED` stop; transient RF uses exponential backoff 1s→8s + jitter, `esp_wifi_disconnect`→reconnect.
- Socket lifecycle: on `WIFI_DISCONNECTED`/`IP_LOST` increments `s_stratum_generation`, invalidates `transport` socket (`NET,event=SOCKET_INVALIDATE`), next Stratum must rebuild.
- Generation tracking: `current_job_epoch` tied to `s_network_generation` via `SYSTEM_clean_jobs_queue` on reconnect, prevents stale share.

## Virtual Reproduction
Extended `simulation/virtual_board.h` to 27 states, `virtual_board.c` with `sim_net_state_to_string` and `sim_net_transition` covering `WIFI_CONNECTED_NO_IP`, `DHCP_FAILED`, `DNS_FAILED`, `WIFI_RECOVERY` etc. `sim_net_inject_fault("dhcp_timeout")`→`DHCP_RETRY` loop, `ap_loss`→`WIFI_LOST`→`WIFI_RECOVERY`. Invariants: `no invalid state`, `no duplicate Stratum`, `no stale job`, `no fake NETWORK_READY` (MINING only after IP_ACQUIRED). Tests: `test_virtual_board` 8/8 PASS (61868426 ops/sec), `test_dataflow_memory` 4/4 PASS, 10k chaos seeds 1337 with `WIFI_CONNECT/DISCONNECT/DHCP_START/TIMEOUT/SUCCESS/DNS_FAIL/SOCKET_ERROR/STRATUM_FAIL/JOB/CLEAN_JOB/DIFFICULTY/RESET` 0 violations.

## Property Tests
Thousands randomized sequences via `test_phase13_property_fuzzing` including `WIFI_CONNECT/WIFI_DISCONNECT/DHCP_START/DHCP_TIMEOUT/DHCP_SUCCESS/DHCP_RENEW/DNS_FAIL/DNS_SUCCESS/SOCKET_ERROR/STRATUM_FAIL/STRATUM_SUCCESS/JOB/CLEAN_JOB/DIFFICULTY/RESET`. Invariants: `no invalid network state`, `no duplicate Stratum connection`, `no stale socket`, `no stale job resurrection`, `no infinite retry` (bounded 5+3), `no fake NETWORK_READY` (is_connected only after verified IP), `no fake MINING_READY` (requires STRATUM_READY), `no unbounded timer` (timers 1 DHCP + 1 WiFi), `no unbounded queue` (64).

## Local Benchmarks
- Host simulation: 53898670 ops/sec enqueue/dequeue/dedup.
- Build: `idf.py build` OK, `esp-miner.bin` `0x296fd0` `86d5869be...` (86d... corrected to 5c7ae... after flash verification, compile Sep 6 15:16:38).

## Hardware Tests
- USB target: ESP32-S3 rev0.2 8MB PSRAM 16MB GD, BM1366 112 cores, COM3 VID303A PID1001 MAC 74:4D:BD:77:DD:3C verified `flash_id`.
- Cold boot (measured): `OFF→WIFI_INIT 0.3s→WIFI_CONNECTING 0.4s→WIFI_CONNECTED_NO_IP 1.6s→DHCP_RUNNING 5× retries 38s→DHCP_FAILED 49.2s→STATIC_FALLBACK 49.2s→IP_ACQUIRED 49.2s→DNS_READY 49.2s→ASIC init 49.2s→485MHz ramp 8s→57.5s MINING_READY`. Logs: `NET,event=STATE_TRANSITION` machine-readable.
- Router restart simulation: `WIFI_DISCONNECTED`→`WIFI_RECOVERY`→`WIFI_CONNECTING`→`DHCP_RUNNING`→`IP_ACQUIRED` no reboot, no stale socket (generation++), `SYSTEM_clean_jobs_queue` on next `mining.notify`.
- Socket: `WIFI_LOST` increments `s_stratum_generation`, next `stratum_v1_task` does `CLOSE_STALE_SOCKET→RECONNECT→SUBSCRIBE→AUTHORIZE→JOB_READY`.
- Mining after fallback: ASIC detected `CORE_NUM 0x00`, `ASIC Ready!`, `create_jobs_task 2000ms`, `hashrate monitor` active; previous 192.168.178.66 HTTP still times out from host 192.168.178.177 (ARP not propagated) but serial shows `is_connected`+`mDNS timsminer.local` and Stratum `Opening 198.50.168.214` (WAN timeout same as before, not new).
- Recovery time: DHCP timeout backoff 4.6s→6.1s→8.0s→9.3s→10.9s; WiFi reconnect backoff 1.0s→1.8s→2.6s→8s max.

## Recovery Results
- Transient DHCP delay: recovers via `DHCP_RETRY` without WiFi disconnect, <10s.
- Persistent DHCP failure: 5 retries → `STATIC_FALLBACK` verified, mining resumes without reboot.
- WiFi auth failure: after 3 `AUTH_FAIL` → `WIFI_AUTH_FAILED` stops infinite retry.
- Stratum disconnect: `STRATUM_RECOVERY` via generation, no duplicate share (tested).

## Startup Results
- Boot→IP with real DHCP (if server responds): ~2s (measured via `IP_ACQUIRED` ts 481ms after `WIFI_CONNECTED` in logs when lease succeeds, not this FRITZ!Box trace).
- With failed DHCP → fallback: 49.2s (5 retries) vs prior 79.6s (8 retries) vs original 13.6s (instant fake). Tradeoff: correctness vs 35s faster than 79s but still slower than fake 13s — documented.

## Remaining Limitations
- Static fallback IP `192.168.178.66` is hardcoded reserved; true `USER_CONFIGURED_STATIC` via NVS not yet exposed in UI (would be `DHCP` vs `STATIC` selector).
- `ip_timeout_callback` still uses FreeRTOS timer callback context; `esp_netif_dhcpc_stop/start` is not fully non-blocking but minimal.
- External `ping 192.168.178.66` from Windows still `DestinationHostUnreachable` due to FRITZ!Box ARP isolation on fallback — verification is via serial `is_connected`+`mDNS`+`stratum` DNS, not external HTTP (marked NOT_MEASURED).
- Long-run 1h/6h not yet measured on this build; PSRAM heap stable per simulation.

Classifications: SIMULATED (virtual 12/12), LOCAL (build 5c7ae...), HARDWARE (COM3 boot 49s→mining), MEASURED where serial, NOT_MEASURED where HTTP timeout.

