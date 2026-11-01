# WLAN backup of the running application

Two read-only routes make the currently running application partition downloadable
over the LAN:

| Route | What it returns |
|---|---|
| `GET /api/system/backup/info` | JSON: partition label, flash offset, length, SHA-256, version, project |
| `GET /api/system/backup/running-app` | the raw bytes of that partition, with the digest in headers |

## What this is

The exact bytes of the partition the device booted from, so the image can be
written back to the same offset unchanged. The digest is computed in a separate
read pass and sent in `X-Image-Sha256` **before** the body, so a client verifies
what it actually received instead of trusting a number delivered afterwards.

```bash
curl -s http://<device>/api/system/backup/info | jq .
curl -sD headers.txt -o running-app.bin http://<device>/api/system/backup/running-app
sha256sum running-app.bin
grep -i x-image-sha256 headers.txt
```

## What this is not

- **Not a full-chip backup.** Bootloader, partition table, `otadata`, NVS
  (Wi-Fi, pool, wallet) and eFuses are outside the running partition. The JSON
  says so in `scope`, and `complete_chip_backup` is `false`.
- **Not a way to read other partitions.** The handler always resolves
  `esp_ota_get_running_partition()` itself; no client-supplied offset, length or
  label is accepted, so it cannot dump NVS or another OTA slot.
- **Not available in AP mode.** The setup access point is open, so both routes
  answer `409` there.
- **Not a restore mechanism.** Writing the downloaded bytes back needs
  `esptool write-flash <offset> running-app.bin` over USB (the offset comes from
  `info`), or the OTA endpoint for an application image. The full-flash USB dump
  remains the primary rescue path.

## Size and time

The route serves the whole partition, including the unused `0xFF` tail, because a
padded image restores exactly as it was read. On this board the application
partition is 4 MiB, so expect roughly 4 MiB per download plus one extra read pass
for the digest.

## Why it exists

The OTA endpoint accepts an application image but nothing in the stock firmware
let an operator copy what was running before flashing something else. The USB
full-flash backup records that, but it needs physical access; this gives a
hash-verified copy over the network, in line with the rule that no physical flash
happens without a restorable copy.
