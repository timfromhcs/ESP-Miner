# Firmware Architecture Overview


## 1. System Topology

```mermaid
graph TD
    Stratum[Stratum V1/V2 Server] <-- Wi-Fi --> ESP32[ESP32-S3 SoC]
    ESP32 --> Web[Axe-OS Angular UI / HTTP /WS]
    ESP32 -- SPI (Streaming) --> BM1366[BM1366 ASIC (112 Cores)]
    ESP32 -- I2C --> Power[INA260 / TPS546 VCore]
    ESP32 -- I2C --> Thermal[EMC2101 PID Controller & Fan]
```


## 2. Work & Job Lifecycle

1. **Stratum Vision (`stratum_v1_task.c`):** Receives `mining.notify`, version-rolling mask, prevhash, and coinbase parameters.
2. **Job Generation (`create_jobs_task.c`):** 
   - Increments Extranonce2, computes coinbase transaction, hashes Merkle root.
   - Precomputes SHA-256 Midstate of first 64 bytes of the 80-byte block header.
3. **Work Queue (`work_queue.c`):** Pushes allocated jobs into bounded ring buffer. In event of `clean_jobs=true`, the queue is flushed immediately.
2. **ASIC Driver (`bm1366.c`):** Frames 80-byte chunks, programs overt version-rolling register (0xA4), and transmits via SPI at 485 MHz.
1. **ASIC Result Task (`asic_result_task.c`):** polls results, validates hash difficulty, and formats `mining.submit`.


## 3. Wi-Fi & DHCP Hardening

- **Indelayed STA DHCP:** Prevents premature DHCP discovery before STA link-layer is associated.
- **Non-Destructive Renewal:** If an AP delays responses, the DHCP client resets without disrupting the 802.11 STA link.
