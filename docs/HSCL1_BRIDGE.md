# HSCL1 research bridge integration boundary

Status: **PLANNED** for transport/receiver integration; the existing software SHA
oracle and research guard are implemented separately in this firmware branch.
This document does not enable a service, change a device, or claim a hardware run.

## Existing surface and evidence semantics

The experimental [research mode](RESEARCH_MODE.md) is off by default. Its HTTP
and USB interfaces expose bounded software SHA-256/SHA-256d and
`hcs.sha.trace.v1` traces. They do not expose BM1366 round state, replace mining
firmware, or provide a distributed solver. Host tests compile the firmware's
`main/research/research_sha.c`, `research_guard.c` and console parser directly.

The HSCL1 side owns the receiver, pairing lifecycle, registry, storage, dataset
exports and CLI/desktop integration. ESP-Miner owns device safety, firmware
capabilities, bounded capture and eventual opt-in transport. CQ-HECS owns trace
replay and solver/proof correctness. No component may turn a software replay
into an ASIC measurement or a heuristic result into a proof.

## Draft wire contract, not an implemented uploader

The initial schema drafts live in HSCL1 under `docs/miner/`:

- [`hcs.miner.event.v1`](https://github.com/timfromhcs/HSCL1/blob/d9a6f020d3a5a9960aea21a9aea78170a2bcd32a/docs/miner/hcs.miner.event.v1.schema.json)
- [`hcs.miner.heartbeat.v1`](https://github.com/timfromhcs/HSCL1/blob/d9a6f020d3a5a9960aea21a9aea78170a2bcd32a/docs/miner/hcs.miner.heartbeat.v1.schema.json)
- [`hcs.miner.batch.v1`](https://github.com/timfromhcs/HSCL1/blob/d9a6f020d3a5a9960aea21a9aea78170a2bcd32a/docs/miner/hcs.miner.batch.v1.schema.json)
- [`hcs.miner.ack.v1`](https://github.com/timfromhcs/HSCL1/blob/d9a6f020d3a5a9960aea21a9aea78170a2bcd32a/docs/miner/hcs.miner.ack.v1.schema.json)

These links identify the reviewed starting revision, not a stable public API.
Do not duplicate and independently modify the schemas in this repository. Before
implementing transport, freeze a compatible schema revision and cross-test
firmware-generated fixtures against the receiver's validator. Existing
`hcs.sha.trace.v1` payloads keep their own format inside any later event envelope.

There is currently no HSCL1 uploader, heartbeat task, durable spool, receiver
discovery or ACK/retry implementation in this firmware. Adding a schema reference
does not implement any of those capabilities.

## Gates before a device transport can be enabled

1. Use an unpredictable pairing secret and a reviewed authenticated provisioning
   flow. MAC addresses, firmware hashes, boot IDs and hostnames are identifiers,
   not secrets. The current six-digit research token is only the documented
   same-LAN/console speed bump; it is not a durable receiver HMAC key.
2. Keep transport off by default and make receiver selection explicit. Pin the
   receiver certificate; fail closed on a bad pin or unavailable authentication.
   Do not introduce automatic LAN scans or upload private configuration.
3. Bound request bytes, queued events, retries and flash writes. Define sequence,
   deduplication, power-loss recovery and ACK semantics before writing a spool;
   do not repurpose existing flash partitions without a reviewed backup plan.
4. Preserve thermal, power, mining and job/time gates. An invalid/negative sensor
   reading marked known must be refused; it must not become a zero or an
   unlimited safety budget. Unknown power remains the existing disclosed
   downgrade, not a fabricated measurement.
5. Prove the exchange with a simulated device and explicit failure cases first.
   Firmware build, simulator, device contact, flashing and hardware validation
   are separate gates. Only the operator can authorize the latter actions.

## Host regression result (2026-11-08)

The numeric-safety regression exposed 13 failures before the fix: negative or
negative-infinite known readings could arm/start research or survive the
watchdog, and an infinite configured power ceiling was accepted. The guard now
requires nonnegative readings within finite configured ceilings.

`simulation/test_research_guard.c` covers `-1`, negative infinity, positive
infinity and NaN for readings and the power ceiling. It exercises arm, begin-job
and watchdog paths. The full host suite built with GCC `-Wall -Wextra -Werror`
and ran successfully, including 143 research-guard assertions. This is host
test evidence, **not hardware validation** or a complete ESP-IDF image build.

Validation commands, run in WSL Ubuntu 26.04.1 from `simulation/`:

```bash
make && make run
make clean
make CFLAGS="-Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -I."
make run
cd ..
bash tools/test_build_release_args.sh
```

Both complete ten-binary host runs and the release-argument checks exited 0;
the sanitizer run reported no ASan/UBSan diagnostics. The generic HSCL1 Verify
detector does not recognize this C/ESP-IDF project (`no runnable checks detected`),
so it supplied no verification result. The explicit Makefile checks above are
the evidence. A clean firmware build and CI remain separate pending gates.
