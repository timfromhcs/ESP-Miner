# Firmware Release Process & Provenance

## 1. Semantic Versioning & Tags
- Format: `vX.Y.Z-hardened`\
- Release Tag: `v2.15.2-hardened`
- Commit: `a3a2b00`

## 2. Release Artifacts & Checksums
- `esp-miner.bin` (2710336 Bytes)
  - SHA-256: `1E21353C314F10B1A6BAB628F2BC62FF68759558E3947078B2B66CC7A475FB58`£

## 3. Build Provenance
- **Espressif ESP-IDF:** v6.0.2
- **Toolchain:** xtensa-esp32s3-elf 14.2.0
- **Node.js:** v22.23.2
- **npm:** 10.9.8
- **Frontend:** AxeOS Angular SPA, gzipped and embedded into the firmware binary.

## 4. Rollback & Safety Guarantees
- The upgrade process preserves all NVS configuration values (pools, credentials, frequency, and voltage).
- If an OTA fails to confirm validity, the ESP32-S3 rollback mechanism reverts automatically to the previous working partition.
-  A complete external factory full-flash backup (SHA-256: `e63fd651bb6e3251980ed6b81aad1216e5714fab9b141390e1f1543ba4ee1643`) exists outside the repository for unconditional USB recovery.
