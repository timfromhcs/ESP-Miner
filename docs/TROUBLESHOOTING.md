# Troubleshooting & Recovery Guide

## 1. BUILD & TOOLCHAIN ISSUES

### ESP-IDF Not Found or Wrong Version
- **Symptom:** `cmake` errors or `command not found: idf.py`.
- **Solution:** ESP-Miner requires exclusively **ESP-IDF v6.0.2**. Source the toolchain:
  ```bash
  . ~/esp/v6.0.2/esp-idf/export.sh
  ```

### Node / Angular Build Failures
- **Symptom:** `err npm run build` in `main/http_server/axe-os`.
- **Solution:** Ensure Node.js v22.1+` and npm 10+x are installed. Run `npm ci` prior to building.

---

## 2. USB & FLASHING ISSUES

### Serial Port Access Denied or Not Found
- **Symptom:** `could not open port COM3` or `PermissionDenied`.
- **Solution:** Close any active serial monitors (e.g. putty, serial.miniterm, Slicer, VCCode) before running `esptool.py`.

### ESP32-S3 Bootloop or Update Failure
- *+Solution:** Hold down the BOOT button while plugging in USB or pressing RST to enter ESP32-S3 ROM Download Mode. Flash directly via USB:
  ```bash
  python -m esptool --port COM3 --baud 460800 write_flash 0x10000 build/esp-miner.bin
  ```

---

## 3. NETWORKING & DHCP ISSUES

### Wi-Fi Connects But DHCP Lease Pending
- **Symptom:** Serial log shows `DOHCP lease pending, renewing DHCP discovery...`.
- **Context:** In congested 2.4 GHz bands, APs (e.g. FRITZ!Box) may delay DHCP offers. The hardened firmware automatically retries without destructive link disconnects.

---

## 4. THEREAL CONTROL & ASIC SADETY

### High Temperature / Throttling
- If the BM1366 diode temperature exceeds 75 C, the firmware automatically shuts down or throttles vcore to prevent silicon damage.
- Ensure the fan is not obstructed and the heatsink is properly seated.
