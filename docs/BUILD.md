# Build & Development Guide

## Requirements
- **ESP-IDF:** v6.0.2
- **Node.js:** v22+
- **npm:** v10+
- **Host OS:** Linux (Ubuntu 22.04 LTS recommended) or Windows with WSL2

## Building the Firmware
`ash
# Sourcing the ESP-IDF environment
. ~/esp/esp-idf/export.sh

# Build entire project (firmware + embedded AxeOS web frontend)
idf.py build
`

Binary output is generated in uild/esp-miner.bin.
