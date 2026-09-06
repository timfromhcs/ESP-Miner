# Axe-OS Web UI & REST API Architecture

**Status:** PROVEN | **Measurement:** MEASURED  
**Framework:** Angular 19+ (Standalone Components, Functional Providers)  
**Embedded Server:** ESP-IDF HTTP Server (`esp_http_server`) + WebSockets  

---

## 1. Web Architecture & Asset Embedding

Axe-OS is a modern, responsive Single Page Application (SPA) embedded directly in the ESP32-S3 firmware:
- **Build Pipeline:** Angular build compiles to optimized static assets, which are GZIP-compressed and converted into C header byte arrays (`axe_os_pages.h`).
- **Zero-SPIFFS Flash Footprint:** Static assets are served directly from flash memory, eliminating SPIFFS filesystem overhead and corruption risks.
- **REST Endpoints:**
  - `GET /api/system/info`: Comprehensive real-time system metrics (hashrate, power, temp, voltage, fan, heap, shares).
  - `POST /api/system/restart`: Graceful reboot trigger with HTTP response confirmation.
  - `POST /api/system/asic`: Dynamic voltage/frequency configuration.
- **WebSocket Streaming:**
  - `/api/system/status`: Real-time telemetry streaming at configurable intervals.
  - `/api/system/log`: Real-time kernel log streaming.

---

## 2. API Security & CORS Policy

- **Private Network Validation:** `is_network_allowed()` restricts API access to private RFC 1918 IPv4 ranges (`10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`) and matching mDNS hostnames (`timsminer.local`), mitigating DNS rebinding attacks.
