#ifndef RESEARCH_SHA_H_
#define RESEARCH_SHA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * FIPS 180-4 SHA-256 as an on-device research oracle.
 *
 * This is deliberately an ordinary, portable C implementation: it has no
 * ESP-IDF dependency, no heap allocation and no global state, so the exact same
 * object file is compiled into the firmware and into the host test binary
 * (simulation/test_research_sha.c). The device can therefore recompute a block
 * a client sent it, and the client can recheck the device, without either side
 * trusting the other.
 *
 * Scope of the claim: this computes SHA-256 and SHA-256d in software on the
 * ESP32-S3 and exposes the per-round values of that SOFTWARE computation. It is
 * not a view into the BM1366. The ASIC executes SHA-256d internally and only
 * returns nonces/targets; no register or buffer of the ASIC is read here, and no
 * claim of "ASIC round access" is made or implied.
 *
 * Trace JSON is byte-identical in structure to the Python reference
 * (`cqhecs/sha/round_trace.py`, schema `hcs.sha.trace.v1`, source
 * `SOFTWARE_REPLAY`), so `cqhecs.sha.round_trace.replay()` can validate a device
 * trace without a special case. Extra provenance belongs in the HTTP envelope,
 * never inside the trace object: `replay()` compares the round objects field by
 * field and an extra key would make a correct trace look tampered with.
 */

#define RESEARCH_SHA_BLOCK_LEN 64
#define RESEARCH_SHA_DIGEST_LEN 32
#define RESEARCH_SHA_STATE_WORDS 8
#define RESEARCH_SHA_ROUNDS 64

typedef struct {
    uint32_t h[RESEARCH_SHA_STATE_WORDS];
} research_sha_state_t;

/* One executed round of a single compression function call. */
typedef struct {
    uint8_t t; /* round index, 0-based */
    uint32_t k;
    uint32_t w;
    uint32_t in[RESEARCH_SHA_STATE_WORDS];
    uint32_t t1;
    uint32_t t2;
    uint32_t out[RESEARCH_SHA_STATE_WORDS];
} research_sha_round_t;

/* A single-block trace. `rounds` points at caller-owned storage: the HTTP
 * handler keeps it on the heap (PSRAM) so a 64-round trace never lands on the
 * 8 KB task stack, and the host test keeps it in a static array. */
typedef struct {
    uint8_t block[RESEARCH_SHA_BLOCK_LEN];
    research_sha_state_t state_in;
    research_sha_state_t state_out;
    uint32_t schedule[RESEARCH_SHA_ROUNDS];
    research_sha_round_t *rounds;
    size_t rounds_capacity;
    size_t rounds_executed;
    bool feed_forward;
} research_sha_trace_t;

/* Emits a chunk of JSON. Returns 0 on success; any non-zero value aborts the
 * trace and is propagated, so a closed client socket stops the work instead of
 * being written into. */
typedef int (*research_sha_write_fn)(void *ctx, const char *data, size_t len);

/* FIPS 180-4 section 5.3 initial hash value. */
void research_sha_state_init(research_sha_state_t *state);

/* Block -> big-endian words + message schedule (FIPS 180-4 section 6.2.2). */
void research_sha_expand(const uint8_t block[RESEARCH_SHA_BLOCK_LEN], uint32_t schedule[RESEARCH_SHA_ROUNDS]);

/* Full 64-round compression with feed-forward. */
void research_sha_compress(research_sha_state_t *state, const uint8_t block[RESEARCH_SHA_BLOCK_LEN]);

/* Complete hash with FIPS padding. No allocation; the tail is built on the
 * stack. `out` may alias nothing: it must be RESEARCH_SHA_DIGEST_LEN bytes. */
void research_sha256(const uint8_t *message, size_t len, uint8_t out[RESEARCH_SHA_DIGEST_LEN]);
void research_sha256d(const uint8_t *message, size_t len, uint8_t out[RESEARCH_SHA_DIGEST_LEN]);

/* One compression under trace. `rounds` must be 0..64; with feed_forward the
 * state is added back (as the real algorithm does), otherwise only the working
 * variables are returned so a caller can inspect a truncated round sequence.
 * Returns 0 on success, -1 on bad arguments, -2 if the capacity is too small. */
int research_sha_trace_run(research_sha_trace_t *trace, const research_sha_state_t *state_in, const uint8_t block[RESEARCH_SHA_BLOCK_LEN],
                           unsigned rounds, bool feed_forward);

/* Serialises the trace as `hcs.sha.trace.v1`. Returns 0 on success, -1 on bad
 * arguments, or whatever the writer returned. */
int research_sha_trace_write_json(const research_sha_trace_t *trace, research_sha_write_fn write, void *ctx);

/* Convenience wrapper for tests and small traces. Returns the number of bytes
 * written excluding the terminator, or -1 on bad arguments, or -2 if the buffer
 * is too small (output is then truncated and NUL-terminated, never partial JSON
 * presented as complete). */
int research_sha_trace_to_buffer(const research_sha_trace_t *trace, char *out, size_t out_len);

/* Lowercase hex helpers. `out_len` is the output capacity in bytes, including
 * the terminator for bytes_to_hex. */
int research_sha_bytes_to_hex(const uint8_t *in, size_t len, char *out, size_t out_len);
int research_sha_hex_to_bytes(const char *hex, uint8_t *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* RESEARCH_SHA_H_ */
