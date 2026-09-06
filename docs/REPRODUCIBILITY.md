# Reproducibility & Autonomous Build Guide

**Status:** PROVEN | **Measurement:** MEASURED  

---

## 1. Toolchain & Prerequisites

To achieve bit-for-bit reproducible builds:
- **Host OS:** Linux (Ubuntu 22.04 LTS / WSL2) or Windows with WSL
- **ESP-IDF:** v6.0.2 (`export.sh` sourced)
- **Toolchain:** `xtensa-esp32s3-elf` GCC 14.2.0
- **Node.js:** v22.x LTS with npm

---

## 2. Build Commands

```bash
# 1. Source ESP-IDF v6.0.2 environment
. ~/esp/esp-idf/export.sh

# 2. Configure build tree
cd /path/to/ESP-Miner
idf.py set-target esp32s3

# 3. Build firmware, bootloader, partition table, and Axe-OS bundle
idf.py build
```

---

## 3. Flashing Target Device (Bitaxe Ultra)

```bash
# 1. Flash directly over verified USB (COM3)
esptool.py --port COM3 --baud 921600 write_flash 0x710000 build/esp-miner.bin

# 2. Or flash full system including bootloader and partition table:
esptool.py --port COM3 --baud 921600 write_flash \
    0x0000 bootloader/bootloader.bin \
    0x8000 partition_table/partition-table.bin \
    0x710000 esp-miner.bin
```
