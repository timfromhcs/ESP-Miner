# OTA Safety

Status as of `v4.0.2-rc1`. Every statement carries an evidence label
(`PROVEN`, `TESTED`, `NOT TESTED`, `MEASURED`, `NOT MEASURED`, ...) as defined in
`GEMINI.md` §0.4.

## 1. What the endpoint does

`POST /api/system/OTA` (`main/http_server/http_server.c:POST_OTA_update`) takes the
raw OTA application image as `application/octet-stream` and streams it into the
next OTA slot. Decision logic lives in `main/http_server/ota_guard.c`, which has no
ESP-IDF dependency and is tested on the host (`simulation/test_ota_guard.c`).

Order of checks — every one before `esp_ota_begin()` leaves flash untouched:

| # | Check | Result on failure | Evidence |
|---|---|---|---|
| 1 | Client inside the allowed local network (`is_network_allowed`) | 401 | PROVEN (code) |
| 2 | Wi-Fi not in AP / APSTA mode | 500 | PROVEN (code) |
| 3 | No other firmware/WWW update in progress | 409 | PROVEN (code), NOT TESTED on target |
| 4 | Next OTA partition exists and is not the running one | 500 | PROVEN (code) |
| 5 | `Content-Length` > 0, ≥ 112 B, ≤ slot size (4 MiB) | 400 / 413 | TESTED (`test_ota_guard`, size gate) |
| 6 | `esp_ota_begin()` succeeds (no longer `ESP_ERROR_CHECK`) | 500, device keeps running | PROVEN (code) |
| 7 | First 112 bytes: image magic `0xE9`, chip id = `CONFIG_IDF_FIRMWARE_CHIP_ID`, app descriptor magic `0xABCD5432`, project name = running app's | 400 + `esp_ota_abort()` | TESTED (synthetic vectors, 200 000 random headers, real v4.0.1 app accepted, real `esp-miner-factory-601-v4.0.1-stable.bin` and a real bootloader rejected) |
| 8 | ≤ 6 consecutive receive timeouts (~30 s with httpd's 5 s default) | 500 + `esp_ota_abort()` | PROVEN (code), NOT TESTED on target |
| 9 | `esp_ota_write()` | 500 + `esp_ota_abort()` | PROVEN (code) |
| 10 | `esp_ota_end()` (ESP-IDF image verification incl. checksum/appended SHA-256) and `esp_ota_set_boot_partition()` | 500, boot partition unchanged | PROVEN (code; ESP-IDF behaviour) |

On every failure path `is_firmware_update` is cleared. Before `v4.0.2-rc1` it was
not, which pinned the display on the firmware-update screen after any failed
upload (`main/screen.c`), and blocked nothing else — now it is also the
concurrency guard, so leaving it set would have locked out further updates.

What the endpoint does **not** do (`NOT PROVEN` / not implemented):

- No authentication beyond the network-range check, no TLS. Never expose it to an
  untrusted network.
- No signature verification.
- No downgrade protection.
- No check that a per-device variant matches the unit (see §3).

## 2. Rollback

| Item | Value | Evidence |
|---|---|---|
| `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` (effective `sdkconfig`) | **not set** | PROVEN (`sdkconfig:699`) |
| `CONFIG_APP_ROLLBACK_ENABLE` | not set | PROVEN (`sdkconfig:5170`) |
| `main.c` `esp_ota_mark_app_valid_cancel_rollback()` | present but **inert**: the bootloader never produces `ESP_OTA_IMG_PENDING_VERIFY` | PROVEN (code + config) |
| Automatic return to the previous image after a failed first boot | **not available** | PROVEN (config) |

Consequence: an image that passes ESP-IDF verification but crashes or cannot
bring up Wi-Fi stays selected. Recovery then needs either

1. the new image to come up far enough to serve `POST /api/system/boot` with
   `{"partition":"<previous slot>"}` (an OTA never overwrites the running slot,
   so the previous image is still there), or
2. USB serial flashing.

Rollback is deliberately not switched on in this release: it is a bootloader
option, an OTA does not replace the bootloader, and enabling it in the app alone
changes the OTA state machine (`ESP_OTA_IMG_NEW` handling) against a bootloader
that does not implement it. That needs its own bench test on a USB-attached unit
before it can be claimed. Tracked as follow-up.

## 3. Which image to flash

| File | Use |
|---|---|
| `build*/esp-miner.bin`, `dist/<variant>/esp-miner-<variant>.bin` | **OTA app image.** The only thing to send to `/api/system/OTA`. |
| `esp-miner-factory-<board>-<ver>.bin`, `esp-miner-merged.bin` | Full flash image (bootloader + partition table + app + www + NVS config) for USB flashing only. Rejected by the OTA endpoint since `v4.0.2-rc1`. |

**Per-device variants must stay per-device.** `tools/build_release.sh tim2` bakes
the station MAC, BSSID pin, static fallback and board identity of the Bitaxe Gamma
at `192.168.178.80`. Flashing the general image onto that unit replaces the pinned
MAC with the eFuse MAC, which moves it out of its DHCP reservation. Flashing the
`tim2` image onto any other unit gives it a duplicate MAC and IP. The OTA endpoint
cannot detect either mistake.

## 4. Operator procedure

1. Read `GET /api/system/info`; confirm `boardVersion`, `ASICModel`, `version`,
   `runningPartition`, `apEnabled == 0`.
2. Compute and record the SHA-256 of the image to send.
3. Upload once:
   ```powershell
   curl.exe --fail-with-body --connect-timeout 5 --max-time 180 `
     -H 'Content-Type: application/octet-stream' `
     --data-binary "@esp-miner.bin" "http://<device>/api/system/OTA"
   ```
4. The device reboots after `Firmware update complete, rebooting now!`. A closed
   connection after the upload is **expected and ambiguous**: do not resend.
5. Poll only the same address with backoff (2, 3, 5, 8, 13, 20, 30 s, ≤ 5 min).
6. Confirm the new `version` and that `runningPartition` changed.
7. If the device does not return: do not flash again blindly. Check the display,
   then recover over USB with the known-good image.
