# Flashing ESP-Miner on a Bitaxe

Two paths exist. Both are documented here because both are supported by the firmware; they differ in
risk, required access and recovery.

> **Read this first.** The OTA endpoint has **no authentication beyond a network-range check, no TLS,
> no signature verification and no downgrade protection** (see [OTA_SAFETY.md](OTA_SAFETY.md)). Never
> expose a miner to an untrusted network, and never flash a host that you do not own.

## 1. USB serial (recovery, first flash, or a bricked device)

Requirements: a data-capable USB cable, `esptool` (`python -m esptool` or the browser flasher
<https://espressif.github.io/esptool-js/>), and the image that matches the board.

| Image | Offset | When |
|---|---|---|
| factory image (`esp-miner-factory-<board>-<version>.bin`) | `0x0` | first flash or full recovery; replaces bootloader, partition table and app **and erases NVS** — Wi-Fi credentials and pool settings are lost |
| app image (`esp-miner.bin`) | `0x10000` | normal firmware update when bootloader and partitions are already correct; keeps NVS |

> Measured 2026-11-09 on board 201: after writing the merged factory image at `0x0`, NVS contained the
> defaults again (`sta.ssid = "myssid"`, placeholder pool) and the device could not rejoin the network.
> A factory flash is a **factory reset**, not an update. For updates keep the app-only path.

```console
# app image on an already-provisioned device
python -m esptool --chip esp32s3 --port COM5 --baud 460800 \
  write_flash -z 0x10000 build/esp-miner.bin

# full factory image (also fixes a damaged bootloader)
python -m esptool --chip esp32s3 --port COM5 --baud 460800 \
  write_flash -z 0x0 esp-miner-factory-401-v4.0.2-rc1.bin
```

If the port does not appear: hold **BOOT**, tap **RESET**, release **BOOT**, then retry.

**Board variants matter.** Flashing the wrong variant replaces the unit's factory MAC with the eFuse
MAC, which moves it out of a DHCP reservation. Check `boardVersion` and `ASICModel` first
(`GET /api/system/info`).

## 2. OTA over the LAN (normal update)

AxeOS exposes `POST /api/system/OTA` with `Content-Type: application/octet-stream`. The device writes
the *other* OTA slot, reboots into it, and keeps the previous slot for recovery.

```console
# 0) record the current state
curl -s http://<device>/api/system/info | python -m json.tool

# 1) hash the image locally
sha256sum esp-miner.bin

# 2) upload once
curl --fail-with-body --connect-timeout 5 --max-time 180 \
  -H 'Content-Type: application/octet-stream' \
  --data-binary @esp-miner.bin http://<device>/api/system/OTA
```

The device answers `Firmware update complete, rebooting now!` and restarts into the new image.

### Verify after the reboot

```console
curl -s http://<device>/api/system/info   # version, boardVersion, ASICModel, macAddr, runningPartition
```

Check three things: the version changed, `boardVersion`/`ASICModel` are unchanged, and `macAddr` is
unchanged. If the MAC changed, you flashed the wrong variant.

### Rollback

An OTA never overwrites the running slot. If the new image does not come up far enough to serve the
API, recover over USB (section 1) or, when it does answer, switch back:

```console
curl -X POST -H 'Content-Type: application/json' \
  -d '{"partition":"<previous slot>"}' http://<device>/api/system/boot
```

Automatic rollback is **not** enabled in this firmware: it is a bootloader option and enabling it in
the app alone changes the OTA state machine (see [OTA_SAFETY.md](OTA_SAFETY.md) §2).

## 3. What this firmware does not protect you from

- A malicious image is accepted: there is no signature check. You are trusting the file you send.
- A downgrade is accepted: there is no version floor.
- Anyone who can reach the device on the LAN can flash it while it is running. Firewall the miner, do
  not port-forward it, and keep it on a network you control.
- A power loss during the write leaves the device in the bootloader; USB recovery is the way back.

HSCL1 users can do the same steps through `hscl miner probe|plan|ota|flash`, which refuse non-private
hosts and require an explicit authorization phrase.
