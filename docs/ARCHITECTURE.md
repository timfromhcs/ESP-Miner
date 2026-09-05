# Firmware Architecture Overview

## 1. System Topology
ESP-Miner operates on an ESP32-S3 dual-core microcontroller orchestrating communication between:
- **Host Network:** Stratum V1/V2 servers via Wi-Fi 802.11 b/g/n.
- **ASIC Subsystem:** Bitmain BM1366 ASIC via high-speed SPI (job payload transmission and serial result polling).
- **Power Subsystem:** TI TPS546 / PMBus power controller adjusting VCore voltage.
- **Thermal Subsystem:** Microchip EMC2101 / EMC2103 PWM fan controller and temperature diode monitor.
- **User Interface:** AxeOS Angular web frontend served over internal HTTP REST/WebSocket server.

## 2. Core Mining Pipeline
1. **Stratum Task (main/tasks/stratum_v1_task.c):** Establishes TCP connection, subscribes, authorizes worker, receives mining.notify.
2. **Job Generation Task (main/tasks/create_jobs_task.c):** Combines coinbase1, extranonce1, extranonce2, coinbase2 to form merkle root. Constructs block header and precomputes SHA-256 midstate.
3. **Work Queue (main/work_queue.c):** Thread-safe ring buffer passing partitioned jobs to the ASIC driver.
4. **ASIC Driver (components/asic/bm1366.c):** Encapsulates SPI framing, sets frequency/voltage registers, configures overt version-rolling mask (0xA4), and streams 80-byte header chunks.
5. **ASIC Result Task (main/tasks/asic_result_task.c):** Polls ASIC response buffer, checks difficulty against stratum target, formats mining.submit, and records telemetry.
