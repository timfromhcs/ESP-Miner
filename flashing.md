## Flashing a Bitaxe

The authoritative guide for this fork is [docs/FLASHING.md](docs/FLASHING.md). It covers both paths:

- **USB serial** (recovery, first flash): factory image at `0x0`, app image at `0x10000`.
- **OTA over the LAN**: `POST /api/system/OTA` with `Content-Type: application/octet-stream`.

Read the safety section before using OTA: there is **no authentication beyond a network-range check, no
TLS, no signature verification and no downgrade protection** ([docs/OTA_SAFETY.md](docs/OTA_SAFETY.md)).

- Always match the image to the board variant (`boardVersion` / `ASICModel` from
  `GET /api/system/info`). A mismatched image replaces the factory MAC with the eFuse MAC.
- Keep miners on a network you control; never port-forward them.
- An OTA keeps the previous slot. USB is the recovery path when the new image does not boot far enough.

For a browser flasher, [esptool-js](https://espressif.github.io/esptool-js/) works with the same
factory images at `0x0`; use the factory image, not the app-only `esp-miner.bin`, when the bootloader or
partition table is in unknown state.
