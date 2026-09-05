# Troubleshooting & Recovery

## Common Issues & Resolutions
1. **DHCP Acquisition Delay on Certain Access Points:**
   - *Symptom:* Wi-Fi connects, but logs show pending DHCP lease.
   - *Resolution:* Firmware includes non-destructive DHCP renewal callback. The connection is held stable until the AP responds with lease parameters.
2. **ASIC Overheating Protection:**
   - *Symptom:* Hashrate throttles or miner pauses.
   - *Resolution:* Check fan unobstructed path. Emergency throttle activates if diode temp exceeds 75°C.
3. **Rollback to Factory Image:**
   - If an OTA or flash fails, the device boots into rollback partition or can be re-flashed via USB using sptool.py with the pre-upgrade backup image.
