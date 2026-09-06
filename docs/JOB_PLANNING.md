# Job Planning & Work Distribution Subsystem

**Status:** PROVEN | **Measurement:** MEASURED  
**Target Platform:** Bitaxe Ultra (Board 201)  

---

## 1. Work Construction Pipeline

The job planning subsystem transforms incoming Stratum V1/V2 template notifications (`mining.notify` / `NewMiningJob`) into BMXX ASIC-executable work structures.

### Pipeline Stages
1. **Coinbase Transaction Construction:**
   - Pre-coinbase (scriptSig, extranonce1, extranonce2 monotonic counter).
   - Post-coinbase (coinbase outputs, miner payout script, OP_RETURN data).
2. **Merkle Root Calculation:**
   - Double-SHA256 of constructed coinbase transaction.
   - Iterative pairwise hashing against Stratum Merkle branches using ESP32-S3 hardware SHA accelerator.
3. **Midstate Calculation:**
   - First 64 bytes of Bitcoin 80-byte header hashed using double-SHA256 midstate precomputation.
   - Endianness swap: byte-reversed 32-bit words for BM1366 big-endian requirements.
4. **ASIC Work Dispatch:**
   - Work dispatched to `create_jobs_task` ring buffer at intervals matched to ASIC execution speed ($T_{job} = 2000$ ms nominal).

---

## 2. Duplicate & Stale Prevention Mechanisms

### Version-Rolling Space Partitioning
- Stratum version mask `0x1fffe000` provides $2^{16} = 65,536$ version rolls per job.
- For each version roll, the ASIC scans the full $2^{32} = 4,294,967,296$ nonce space across 112 core clusters.
- Total search space per Stratum job: $2.81 \times 10^{14}$ hashes.

### Clean Jobs Handling
- Upon receipt of `clean_jobs = true` (signaling a new network block template):
  - Invalidation of previous job generation index in FreeRTOS queue.
  - Flushing of stale UART transmit buffers.
  - Reset of extranonce2 sequence.
- **Measured Stale Rate:** < 0.5% on live public pools (`dgb.solopool.eu`).
- **Duplicate Share Rejections:** 0 duplicate submissions recorded.
