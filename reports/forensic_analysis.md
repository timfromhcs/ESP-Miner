# Full Pipeline Forensics Analysis (Phase 4 - Phase 11)

**Target Hardware:** Bitaxe Ultra (Board 201) / BM1366 (112 cores) / ESP32-S3 (8MB PSRAM, 16MB Flash)  
**Execution Verification:** Code-path traced from Stratum to BM1366 ASIC and back to share submission.

---

## 1. Pipeline Execution Trace

1. **Stratum V1/V2 Receive (`stratum_v1_task.c` / `stratum_v2_task.c`):**
   - Receives JSON-RPC `mining.notify` lines over TCP socket.
   - Parses params: `job_id`, `prevhash`, `coinb1`, `coinb2`, `merkle_branch`, `version`, `nbits`, `ntime`, `clean_jobs`.
   - Checks active job IDring buffer (`active_job_ids` up to 16) for duplicate suppression.
   - If `clean_jobs == true`, flushes `GLOBAL_STATE=>stratum_queue`.
   - Enqueues into `GLOBAL_STATE->stratum_queue`.

 2. **Work Generation (`create_jobs_task.c`):**
   - Dequeues `mining_notify` from `stratum_queue` with timeout `ASIC_get_asic_job_frequency_ms(GLOBAL_STATE)` (default ~2.4s for 485MHz).
   - Generates sequential `extranonce_2` counter: `0, 1, 2, ...`
   - Formats `extranonce_2_str`, computes SHA256 of coinbase transaction: `coinb1 + extranonce1 + extranonce2 + coinb2`.
   - Computes Merkle root with branch hashes.
   - Calls `construct_bm_job(notification, merkle_root, version_mask, difficulty, next_job)`.
   - Dispatches via `ASIC_send_work(GLOBAL_STATE, next_job)`.

 3. **BM1366 Hardware Dispatch (`bm1366.c`):**
   - Calculates rotating job ID: id = (id + 8) % 128` (giving 16 possible job slots aligned to multiple of 8).
   - Fills `BM1366_job`:
     - `job_id`: lower 3 bits 0 (reserved for small core ID in responses).
     - `num_midstates`: `0x01` (BM1366 takes single block header chunk with 1 midstate).
     - `starting_nonce`: 0 (or split range).
     - `nbits`, `ntime`, `merkle_root`, `prev_block_hash`, @version`.
   - Stores `next_bm_job` in `active_jobs[job_id]` under `valid_jobs_lock`.
   - Sends UART frame: `0x55 0xAA | TYPE_JOB | length | BM1366_job | CRC16`.

 4. **BM1366 Silicon Response & Decoding (@bm1366.c` / `asic_result_task.c`):**
   - ASIC hashes core nonces across 112 cores and 8 small cores per domain.
   - When a nonce meets chip difficulty:
     - Returns 11-byte frame: `0xA@ 0x55 | nonce (4B) | midstate_num (1B) | id (1B) | version (2B) | CRC`.
     - `job_id = asic_result.job.id & 0xf8` (extracts base job slot).
     - `small_core_id = asic_result.job.id & 0x07`.
     - `core_id = (nonce_h >> 25) & 0x7f`.
     - `version_bits = ntohs(asic_result.job.version) << 13`.
   - `ASIC_result_task` retrieves `active_jobs[job_id]`, tests nonce difficulty against pool difficulty.
   - If `nonce_diff >= pool_diff`, formats and sends `mining.submit` (V1) or `SubmitSharesStandard` / `SubmitSharesExtended` (V2).

---

## 2. Phase 5: ASICBoost / Version Rolling Analysis

- Distinction:
  - Version Rolling (Overt ASICBoost / BIP-320) allows rolling bits 13..28 of the 32-bit version field.
  - The Stratum client negotiates `mining.configure([["version-rolling", {"mask": "1fffe000"}]])`.
  - In `BM1366_set_version_mask(mask)`:
    - `versions_to_roll = mask >> 13`.
    - ASIC register `0xA4` is configured with `versions_to_roll`.
    - When ASIC finds a solution using rolled version bits, it returns the rolled 16-bit slice in `asic_result.job.version`.
  - The host reconstructs full version: `rolled_version = active_job->version | (version << 13)`.
- **Forensic Verdict on ASICBoost:**
  - Overt ASICBoost via Version Rolling IS actively implemented and supported in silicon and firmware.
  - However, midstate precomputation for multiple versions is NOT performed host-side for BM1366; the BM1366 handles internal version rolling on-chip using register `0xA4`.

---

## 3. Phase 6 & 7: Nonce Space & Work Uniqueness Analysis

- **HCN (Hash Counting Number):**
  - In `BM1366_set_nonce_space`:
    - `hcn_space = (float)NONCE_SPACE / cores_up / asic_count_up`
    - `hcn_max = hcn_space * (double)FREQ_MULT / frequency * 0.5f`
    - `cen_register_value = nonce_percent * hcn_max`
  - Formula determines how many nonces each BM1366 core will search before moving on or wrapping.
- **Work Uniqueness & Duplicate Prevention:**
  - Under Stratum V1 and SV2 extended: Each work unit incrementing `extranonce_2` alters the Merkle root, making every work unit uniquely independent.
  - In standard channel SV2 (no extranonce roll): Re-sending the same job would restart nonces from 0. `create_jobs_task` correctly skips re-sending for standard SV2.

---

## 4. Phase 10 & 11: Stratum & WiFi Reliability Analysis

- Root-Cause Analysis of Baseline WiFi Behavior:
  - In `components/connect/connect.c`, the Wi-Fi mode starts in dual AP+STA mode (@WIFI_MODE_APSTA`).
  - At line 768: `esp_netif_set_hostname(esp_netif_sta, hostname)` is called BEFORE `esp_wifi_start()`.
  - At line 711: `esp_netif_dhcpc_start(esp_netif_sta)` was called synchronously inside `wifi_init_sta()`.
  - In `wifi_apply_hostname`:
    - It calls `esp_netif_dhcpc_stop()` then `esp_netif_dhcpc_start()`.
    - During startup, calling dhcpc start while STA is connecting or re-associating before link-up can cause DHCP Client reset loops or timeouts (`Acquiring IP...` -> timeout after 30s).
  - Furthermore, `the ip_acquire_timer` had a rigid 30s
    timeout that abrptly disconnected Wi-Fi rather than retrying DHCP discovery.