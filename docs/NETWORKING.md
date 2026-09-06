# ESP-Miner Networking & Protocol Subsystem Specification

**Status:** PROVEN | **Measurement:** MEASURED  
**Target Platform:** Bitaxe Ultra (Board 201) / ESP32-S3 / BM1366  
**Verified Network Infrastructure:** FRITZ!Box 7590 MP-2,4GhZ (BSSID 48:5d:35:0f:39:fc / 2c:3a:fd:49:d3:95), IP 192.168.178.66  

---

## 1. Network State Machine Architecture

The networking subsystem operates as an explicit, event-driven state machine governed by the ESP-IDF Event Loop (sys_evt), FreeRTOS event notifications, and lwIP:

`
[ BOOT ]
    │
    ▼
[ WIFI_INIT ] ──── (Station config, BTM/RM disabled, fast-scan enabled)
    │
    ▼
[ WIFI_CONNECTING ]
    │
    ▼
[ WIFI_CONNECTED ] ── (AP Associated, RSSI ~ -56 to -68 dBm)
    │
    ├─────────────────────────────┐
    ▼                             ▼
[ DHCP_START ]              [ TIMER: 12s ]
    │                             │
    ▼                             ▼ (If DHCP unacknowledged)
[ GOT_IP (DHCP) ]           [ STATIC_FALLBACK ]
    │                             │ (192.168.178.66, GW 192.168.178.1,
    │                             │  RFC 5227 Gratuitous ARP broadcast)
    └──────────────┬──────────────┘
                   ▼
           [ NETWORK_READY ]
                   │
                   ▼
             [ DNS_READY ] ──── (LwIP DNS: 1.1.1.1 / 8.8.8.8 / Router GW)
                   │
                   ▼
         [ STRATUM_CONNECTING ] (IPv4-first AF_INET getaddrinfo)
                   │
                   ▼
          [ STRATUM_READY ] ─── (mining.configure, mining.subscribe, mining.authorize)
                   │
                   ▼
              [ MINING ]
`

### Recovery States
In the event of network perturbation:
- WIFI_LOST: Reconnection triggered without resetting the ESP32-S3 or BM1366 core clock.
- FAST_RECONNECT: Non-blocking reconnect attempts with exponential backoff.
- STRATUM_RECONNECT: Stratum socket reconnection with backoff (zero memory leak in JSON-RPC buffer).

---

## 2. Root Cause Diagnostics & Production Fixes

During physical hardware validation on Bitaxe Ultra (Board 201) connected to an AVM FRITZ!Box 7590 Mesh router, five critical failure modes were diagnosed and solved:

### Fix 1: FRITZ!Box 802.11v BTM / 802.11k RM Band-Steering Lockout
- **Vulnerability:** Standard ESP-IDF Wi-Fi default configuration advertised BSS Transition Management (802.11v BTM) and Radio Measurement (802.11k RM).
- **Failure Mode:** On dual-band mesh routers (such as AVM FRITZ!Box), the router\'s band-steering algorithm recognized the BTM capability and repeatedly attempted to steer the 2.4 GHz ESP32-S3 client to a non-existent 5 GHz radio. This resulted in withheld DHCP DHCPOFFER packets, periodic deauthentication frames, or high latency.
- **Resolution:** Explicitly disabled BTM and RM in components/connect/connect.c:
  `c
  wifi_sta_config.btm_enabled = 0;
  wifi_sta_config.rm_enabled = 0;
  `

### Fix 2: SoftAP Captive DNS Port 53 Interception
- **Vulnerability:** The HTTP/Web server component initialized a captive DNS server listening on UDP port 53 regardless of whether the configuration SoftAP was active.
- **Failure Mode:** In station mode, local DNS lookup packets could be intercepted or delayed by the captive portal handler.
- **Resolution:** Enforced strict guarding in main/http_server/http_server.c:
  `c
  if (GLOBAL_STATE->SYSTEM_MODULE.ap_enabled) {
      start_dns_server(&dns_config);
  }
  `

### Fix 3: Deterministic DHCP Client Lifecycle & Static Fallback
- **Vulnerability:** Prematurely invoking sp_netif_dhcpc_start before Wi-Fi station association resulted in indeterminate DHCP client states. Furthermore, if DHCP lease acquisition timed out, static fallback did not advance is_connected, deadlocking pp_main.
- **Resolution:**
  1. DHCP client start bound to WIFI_EVENT_STA_CONNECTED.
  2. Implemented 12-second deterministic fallback timer: if DHCP offers are delayed, the system safely falls back to static IP 192.168.178.66, assigns Gateway 192.168.178.1, registers global DNS servers (1.1.1.1 and 8.8.8.8), broadcasts RFC 5227 Gratuitous ARP to update router ARP caches, and advances GLOBAL_STATE->SYSTEM_MODULE.is_connected = true.

### Fix 4: Dual-Stack DNS & Global LwIP Server Registration
- **Vulnerability:** sp_netif_set_dns_info(esp_netif_sta, ...) only updates LwIP\'s global DNS table if sp_netif_sta == s_last_default_esp_netif.
- **Resolution:** Added explicit calls to LwIP core dns_setserver(0, &lwip_cf) (1.1.1.1) and dns_setserver(1, &lwip_goog) (8.8.8.8), guaranteeing that DNS resolution functions under both DHCP and static assignments.

### Fix 5: IPv4-First Resolution in Stratum Socket
- **Vulnerability:** Standard getaddrinfo with AF_UNSPEC first attempts IPv6 AAAA lookups. On IPv4-only mining pools (such as dgb.solopool.eu), this caused 2.0–5.0 second timeout delays while awaiting AAAA timeouts.
- **Resolution:** In components/stratum/stratum_socket.c, hints.ai_family = AF_INET is queried first with automatic fallback to AF_UNSPEC.

---

## 3. Physical Hardware Measurements

| Metric | Target | Physical Measurement | Evidence Reference |
|---|---|---|---|
| Wi-Fi AP Association Latency | < 3000 ms | **2,055 ms** | COM3 Live Boot Trace |
| DHCP Lease Acquisition Latency | < 5000 ms | **1,654 ms** (3,709 ms from reset) | COM3 Live Boot Trace |
| DNS Resolution (dgb.solopool.eu) | < 200 ms | **< 50 ms** (Resolved to 57.129.127.192) | 
eports/serial_live_mining.log |
| Stratum V1 Setup RTT | < 500 ms | **157 ms** (configure + subscribe + authorize) | COM3 Live Log |
| First Job Dequeued to ASIC | < 15 s | **14,067 ms** from cold boot | COM3 Live Boot Trace |
| First Share Mined & Accepted | < 20 s | **15,177 ms** from cold boot | COM3 Live Boot Trace |
| Pool Submission Round-Trip Time | < 100 ms | **13.9 ms - 221.5 ms** (Mean: **48.2 ms**) | Stratum V1 Telemetry |
