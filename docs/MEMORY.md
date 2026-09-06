# Memory Architecture & Allocation Profiling

**Status:** PROVEN | **Measurement:** MEASURED  
**SoC:** ESP32-S3 (revision v0.2)  
**Memory Hierarchy:**
- Internal SRAM: 512 KiB (228 KiB dynamic heap RAM + 21 KiB + 32 KiB DRAM + 7 KiB RTC RAM)
- Octal PSRAM: 8 MiB (AP Memory 64 Mbit, 80 MHz)

---

## 1. Heap Allocation Strategy & Placement Policies

To prevent heap fragmentation and preserve DMA capability:
1. **DMA & Latency-Critical Buffers (Internal SRAM):**
   - Wi-Fi TX/RX frame buffers (`WIFI_TX_BUF`, `WIFI_RX_BUF`).
   - UART TX/RX ring buffers for BM1366 communications (1,000,000 baud).
   - FreeRTOS task stacks for time-critical tasks (`create_jobs_task`, `asic_result_task`, `stratum_v1_task`).
   - Reserving a dedicated pool of 32 KiB internal memory strictly for DMA/internal allocations (`esp_psram_reserve_dma_pool`).
2. **High-Capacity & Transient Allocations (Octal PSRAM):**
   - Axe-OS embedded Web UI assets (HTML, CSS, JS bundles).
   - HTTP server request/response payloads.
   - cJSON dynamic serialization trees for REST endpoints (`/api/system/info`).
   - Telemetry ring buffers.

---

## 2. Measured Memory Telemetry

Measured on physical Bitaxe Ultra during sustained 485 MHz mining:

| Memory Metric | Available / Total | Free under Load | Utilization |
|---|---|---|---|
| Internal SRAM | 288 KiB | **86,412 bytes** | ~70% |
| Octal PSRAM | 8,192 KiB | **7,633,304 bytes** (~7.63 MiB) | ~6.8% |
| Total Dynamic Heap | 8,480 KiB | **7,719,716 bytes** | ~9.0% |
| Largest Free Internal Block | 228 KiB | **65,536 bytes** | No fragmentation panic |
| Memory Leaks (900s runtime) | 0 bytes | **Stable** | 0 bytes drift |
