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
[ DHCP_START ]              [ TIMER: 8s watchdog ]
    │                             │
    ▼                             ▼ (If DHCP unacknowledged)
[ GOT_IP (DHCP) ]           [ DHCP_RETRY x5 ]
    │                             │ jittered backoff ~5.2s .. 10.0s
    │                             ▼ (retries exhausted)
    │                      [ DHCP_RECOVERY ] x3
    │                             │ 5-7s backoff
    │                             ▼
    │                      [ WIFI_RECOVERY ] -> esp_wifi_disconnect()
    │                             │ re-associate, DHCP again
    │                             └──► (loop until a real lease arrives)
    └──────────────┬──────────────┘
                   ▼
           [ NETWORK_READY ]
                   │
                   ▼
             [ DNS_READY ] ──── (DNS from the DHCP lease)
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

> **There is deliberately no static-IP fallback.** See §3.

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

### Fix 3: Deterministic DHCP Client Lifecycle & Conflict Handling
- **Vulnerability:** Prematurely invoking `esp_netif_dhcpc_start` before Wi-Fi station association resulted in indeterminate DHCP client states. Bouncing the client `stop`→`start` to "retry" reset the DHCP xid and is itself a cause of lease conflicts on some routers.
- **Resolution:**
  1. DHCP client start bound to `WIFI_EVENT_STA_CONNECTED`, and only when its status is `ESP_NETIF_DHCP_STOPPED`. No bounce.
  2. A valid lease is **never wiped** on re-association. `esp_netif_set_ip_info()` requires the DHCP client to be stopped first; zeroing the IP forced a needless renegotiation on every roam, dropping the stratum socket and the web UI.
  3. Retry uses jittered backoff (~5.2 s → 10.0 s over 5 attempts) and lets LwIP retransmit internally per RFC 2131 rather than restarting the client.
  4. After 3 exhausted cycles the station disconnects and re-associates. This repeats until a real lease arrives.

### Fix 3a: Lease conflicts — use the DHCP client, do not spoof the MAC
- **Vulnerability:** an earlier revision overrode the STA MAC to a hardcoded `74:4D:BD:77:DD:3D` and forced the DHCP hostname to `bitaxe-dd3c` to dodge a lease conflict. That was wrong on four counts: it defeats the router-side reservation that assigns `192.168.178.66`; it is global rather than per-device, so two units would collide; the hostname and the MAC contradicted each other; and `esp_wifi_set_mac()` was called *before* `esp_wifi_start()` against the documented contract, with the return value discarded — which is why the real MAC still appears in the device's own API output.
- **Resolution:** the efuse factory MAC is left untouched, and conflict handling is delegated to the DHCP client, which already supports it:

  ```
  CONFIG_LWIP_DHCP_DOES_ARP_CHECK=y
  ```

  > *Sends two ARP probes and only declines the offer if a reply for the offered IP comes from a **different MAC address than the interface MAC**. This is fast (about 1–2 seconds) and avoids false conflicts on networks where the AP echoes the client's MAC in ARP replies.*

  ESP-IDF's docs explicitly warn **against** the obvious alternative, `CONFIG_LWIP_DHCP_DOES_ACD_CHECK`, for exactly this class of router:

  > *Some access points respond to ARP probes with the client's own MAC for the offered IP; upstream behavior treats any matching sender IP during PROBING as a conflict, **which can cause repeated DHCP DECLINEs on such networks**.*

  An AVM FRITZ!Box that echoes the client MAC in ARP replies is precisely that case, so enabling ACD would have made the original symptom worse. The project stays on the ARP check.

- **`CONFIG_LWIP_DHCP_RESTORE_LAST_IP` is deliberately left off.** Caching and restoring the last lease across a power cut is attractive for fast recovery, but the restore path bypasses the ARP conflict check. For a device whose address is already reserved that is the wrong trade.

### Fix 4: DNS
- DNS servers come from the DHCP lease. Earlier revisions also poked LwIP's global DNS table directly via `dns_setserver()` to hardcode `1.1.1.1` / `8.8.8.8`; that was only needed to support the static-IP fallback and has been removed with it.

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
