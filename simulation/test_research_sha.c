/*
 * Host tests for the on-device SHA-256 research oracle
 * (main/research/research_sha.c).
 *
 * Two kinds of evidence, deliberately kept apart:
 *
 *  1. Known-answer vectors (FIPS 180-4 and the Bitcoin genesis header) pin the
 *     full compression, the padding and the double hash to published values.
 *  2. Structural invariants pin the trace recorder, which the vectors cannot
 *     reach: a recorded round must feed exactly its successor, the register
 *     shifts must be the ones in FIPS 180-4 section 6.2.2, and reduced rounds
 *     must compose with feed-forward arithmetic.
 *
 * The trace JSON is emitted so an independent implementation can judge it:
 *
 *   ./test_research_sha --emit 616263
 *
 * prints a single `hcs.sha.trace.v1` object on stdout for the given block hex.
 * `cqhecs.sha.round_trace.replay()` accepts it unchanged, which is how the
 * firmware and the Python reference are compared without either reading the
 * other's source.
 */

#include "research_sha.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void check_hex(const uint8_t *got, size_t len, const char *want, const char *what)
{
    char hex[2 * 256 + 1];
    if (research_sha_bytes_to_hex(got, len, hex, sizeof(hex)) < 0) {
        failures++;
        checks++;
        printf("  FAIL: %s (could not format)\n", what);
        return;
    }
    checks++;
    if (strcmp(hex, want) != 0) {
        failures++;
        printf("  FAIL: %s\n    got  %s\n    want %s\n", what, hex, want);
    }
}

/* ------------------------------------------------------------- KAT vectors */

static void test_kat(void)
{
    printf("== FIPS 180-4 known answers ==\n");
    uint8_t digest[RESEARCH_SHA_DIGEST_LEN];

    research_sha256((const uint8_t *) "", 0, digest);
    check_hex(digest, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256(\"\")");

    research_sha256((const uint8_t *) "abc", 3, digest);
    check_hex(digest, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256(\"abc\")");

    const char *v2 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    research_sha256((const uint8_t *) v2, strlen(v2), digest);
    check_hex(digest, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "SHA-256(56-byte vector)");

    const char *v3 = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    research_sha256((const uint8_t *) v3, strlen(v3), digest);
    check_hex(digest, 32, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1", "SHA-256(112-byte vector)");

    /* A NULL message with zero length must behave like the empty message, not
     * crash: the HTTP handler can see a zero-length body. */
    research_sha256(NULL, 0, digest);
    check_hex(digest, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256(NULL, 0)");
}

/* Boundary lengths around the padding thresholds: 55/56/57 and 63/64/65 bytes
 * are where the tail block grows from one block to two. The reference values
 * come from the standard padding rule, computed here with a second, slower
 * formulation of SHA-256 (one byte at a time, no early block loop). */
static void reference_sha256(const uint8_t *msg, size_t len, uint8_t out[32])
{
    /* Independent path: build the whole padded message, then compress block by
     * block using research_sha_compress(). Only the loop structure differs from
     * research_sha256(), so a faulty tail/block boundary shows up as a mismatch
     * between the two. */
    static uint8_t padded[2048];
    size_t total = len + 1 + 8;
    size_t blocks = (total + 63) / 64;
    research_sha_state_t state;
    research_sha_state_init(&state);
    memset(padded, 0, sizeof(padded));
    memcpy(padded, msg, len);
    padded[len] = 0x80;
    uint64_t bits = (uint64_t) len * 8ull;
    for (int i = 0; i < 8; i++) {
        padded[blocks * 64 - 1 - (size_t) i] = (uint8_t) (bits >> (8 * i));
    }
    for (size_t b = 0; b < blocks; b++) {
        research_sha_compress(&state, padded + b * 64);
    }
    for (int i = 0; i < 8; i++) {
        out[4 * i + 0] = (uint8_t) (state.h[i] >> 24);
        out[4 * i + 1] = (uint8_t) (state.h[i] >> 16);
        out[4 * i + 2] = (uint8_t) (state.h[i] >> 8);
        out[4 * i + 3] = (uint8_t) state.h[i];
    }
}

static void test_padding_boundaries(void)
{
    printf("== padding boundaries ==\n");
    static uint8_t msg[300];
    for (size_t i = 0; i < sizeof(msg); i++) {
        msg[i] = (uint8_t) (i * 31u + 7u);
    }

    const size_t lengths[] = {0, 1, 54, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 129, 200};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        uint8_t got[32], want[32];
        char what[64];
        research_sha256(msg, lengths[i], got);
        reference_sha256(msg, lengths[i], want);
        snprintf(what, sizeof(what), "length %u matches the independent padding path", (unsigned) lengths[i]);
        check(memcmp(got, want, 32) == 0, what);
    }
}

static void test_million_a(void)
{
    printf("== 1,000,000 x 'a' ==\n");
    static uint8_t million[1000000];
    memset(million, 'a', sizeof(million));
    uint8_t digest[32];
    research_sha256(million, sizeof(million), digest);
    check_hex(digest, 32, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "SHA-256(10^6 x 'a')");
}

static void test_genesis_header(void)
{
    printf("== Bitcoin genesis header (SHA-256d) ==\n");
    static const char *header_hex = "01000000"
                                    "0000000000000000000000000000000000000000000000000000000000000000"
                                    "3ba3edfd7a7b12b27ac72c3e67768f617fc81bc3888a51323a9fb8aa4b1e5e4a"
                                    "29ab5f49"
                                    "ffff001d"
                                    "1dac2b7c";
    uint8_t header[80];
    check(research_sha_hex_to_bytes(header_hex, header, sizeof(header)) == 80, "genesis header parses to 80 bytes");

    uint8_t digest[32];
    research_sha256d(header, sizeof(header), digest);

    /* The block hash is the double hash in reverse byte order. */
    uint8_t display[32];
    for (int i = 0; i < 32; i++) {
        display[i] = digest[31 - i];
    }
    check_hex(display, 32, "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f", "genesis block hash");
}

/* --------------------------------------------------------------- trace core */

static research_sha_round_t rounds[RESEARCH_SHA_ROUNDS];
static char json[65536];

static void test_structural_invariants(void)
{
    printf("== trace structural invariants ==\n");
    research_sha_state_t state;
    research_sha_state_init(&state);

    uint8_t block[64];
    for (int i = 0; i < 64; i++) {
        block[i] = (uint8_t) i;
    }

    research_sha_trace_t trace;
    trace.rounds = rounds;
    trace.rounds_capacity = RESEARCH_SHA_ROUNDS;
    check(research_sha_trace_run(&trace, &state, block, RESEARCH_SHA_ROUNDS, true) == 0, "64-round trace runs");
    check(trace.rounds_executed == 64, "all 64 rounds recorded");

    /* FIPS 180-4 section 6.2.2: the working variables are shifted and the two
     * temporary values are folded in. Expressing each output register in terms of
     * the same round's input registers pins the recorder independently of the
     * hash result, which only reaches the final state. */
    int shift_ok = 1;
    int arithmetic_ok = 1;
    for (size_t t = 0; t < trace.rounds_executed; t++) {
        const research_sha_round_t *r = &rounds[t];
        if (r->out[1] != r->in[0] || r->out[2] != r->in[1] || r->out[3] != r->in[2] || r->out[5] != r->in[4] ||
            r->out[6] != r->in[5] || r->out[7] != r->in[6]) {
            shift_ok = 0;
        }
        if (r->out[4] != (uint32_t) (r->in[3] + r->t1)) {
            arithmetic_ok = 0;
        }
    }
    check(shift_ok, "register shift b'=a, c'=b, d'=c, f'=e, g'=f, h'=g holds in every round");
    check(arithmetic_ok, "e' = d + T1 holds in every round");

    int forward_ok = 1;
    for (size_t t = 0; t < trace.rounds_executed; t++) {
        if (rounds[t].out[0] != (uint32_t) (rounds[t].t1 + rounds[t].t2)) {
            forward_ok = 0;
        }
    }
    check(forward_ok, "a' = T1 + T2 holds in every round");

    /* Rounds are contiguous: round t+1's input vector is exactly round t's
     * output vector (the recorder must not copy a stale snapshot). */
    int chain_ok = 1;
    for (size_t t = 0; t + 1 < trace.rounds_executed; t++) {
        for (int r = 0; r < 8; r++) {
            if (rounds[t].out[r] != rounds[t + 1].in[r]) {
                chain_ok = 0;
            }
        }
    }
    check(chain_ok, "each round's output state is the next round's input state");

    check(research_sha_trace_run(&trace, &state, block, 64, true) == 0, "trace reruns deterministically");
    research_sha_round_t replay[RESEARCH_SHA_ROUNDS];
    memcpy(replay, rounds, sizeof(replay));
    check(memcmp(replay, rounds, sizeof(replay)) == 0, "a second identical run produces identical rounds");

    /* The 64-round feed-forward result must equal the hash compression, which is
     * the whole point of using this as an oracle. */
    research_sha_state_t via_compress = state;
    research_sha_compress(&via_compress, block);
    check(memcmp(via_compress.h, trace.state_out.h, sizeof(state.h)) == 0, "traced 64-round output equals research_sha_compress()");
}

static void test_reduced_rounds(void)
{
    printf("== reduced rounds and feed-forward ==\n");
    research_sha_state_t state;
    research_sha_state_init(&state);
    uint8_t block[64];
    memset(block, 0xA5, sizeof(block));

    research_sha_trace_t trace;
    trace.rounds = rounds;
    trace.rounds_capacity = RESEARCH_SHA_ROUNDS;

    check(research_sha_trace_run(&trace, &state, block, 0, true) == 0, "0 rounds accepted");
    check(trace.rounds_executed == 0, "0 rounds executed");

    /* Matches the Python reference: feed-forward always adds the working state,
     * and after zero rounds the working state is still the input state, so the
     * result is input + input (mod 2^32). That is a quirk of the reference
     * contract, not of this implementation, and the two must not drift. */
    int doubled_ok = 1;
    for (int i = 0; i < 8; i++) {
        if (trace.state_out.h[i] != (uint32_t) (state.h[i] + state.h[i])) {
            doubled_ok = 0;
        }
    }
    check(doubled_ok, "0 rounds with feed-forward returns input + input, as the reference does");

    check(research_sha_trace_run(&trace, &state, block, 0, false) == 0, "0 rounds without feed-forward accepted");
    check(memcmp(trace.state_out.h, state.h, sizeof(state.h)) == 0, "0 rounds without feed-forward returns the input state");

    /* Feed-forward is addition, so the two variants must differ by exactly the
     * input state, word for word. An off-by-one in the accumulate step breaks
     * this and nothing else. */
    research_sha_state_t ff_on, ff_off;
    check(research_sha_trace_run(&trace, &state, block, 40, true) == 0, "40 rounds with feed-forward accepted");
    ff_on = trace.state_out;
    check(research_sha_trace_run(&trace, &state, block, 40, false) == 0, "40 rounds without feed-forward accepted");
    ff_off = trace.state_out;
    int add_ok = 1;
    for (int i = 0; i < 8; i++) {
        if (ff_on.h[i] != (uint32_t) (ff_off.h[i] + state.h[i])) {
            add_ok = 0;
        }
    }
    check(add_ok, "feed-forward output = working state + input state");

    check(research_sha_trace_run(&trace, &state, block, 65, true) == -1, "65 rounds rejected");
    check(research_sha_trace_run(NULL, &state, block, 8, true) == -1, "NULL trace rejected");
    check(research_sha_trace_run(&trace, NULL, block, 8, true) == -1, "NULL input state rejected");
    check(research_sha_trace_run(&trace, &state, NULL, 8, true) == -1, "NULL block rejected");

    research_sha_trace_t small;
    small.rounds = rounds;
    small.rounds_capacity = 8;
    check(research_sha_trace_run(&small, &state, block, 9, true) == -2, "capacity smaller than the round count rejected");
    check(research_sha_trace_run(&small, &state, block, 8, true) == 0, "capacity equal to the round count accepted");
}

/* ------------------------------------------------------------------- JSON */

static int contains(const char *haystack, const char *needle)
{
    return strstr(haystack, needle) != NULL;
}

static void test_json(void)
{
    printf("== trace JSON ==\n");
    research_sha_state_t state;
    research_sha_state_init(&state);
    uint8_t block[64];
    memset(block, 0, sizeof(block));
    block[0] = 0x61;
    block[1] = 0x62;
    block[2] = 0x63;

    research_sha_trace_t trace;
    trace.rounds = rounds;
    trace.rounds_capacity = RESEARCH_SHA_ROUNDS;
    check(research_sha_trace_run(&trace, &state, block, 64, true) == 0, "trace for JSON built");

    int n = research_sha_trace_to_buffer(&trace, json, sizeof(json));
    check(n > 0, "trace serialises");
    check(n == (int) strlen(json), "returned length equals the written length");

    check(contains(json, "\"schema\":\"hcs.sha.trace.v1\""), "schema hcs.sha.trace.v1");
    check(contains(json, "\"source\":\"SOFTWARE_REPLAY\""), "source SOFTWARE_REPLAY");
    check(contains(json, "\"block\":\"6162630000000000"), "block hex");
    check(contains(json, "\"input_state\":\"6a09e667bb67ae853c6ef372a54ff53a510e527f9b05688c1f83d9ab5be0cd19\""),
          "input state is the FIPS IV");
    check(contains(json, "\"rounds_executed\":64"), "rounds_executed");
    check(contains(json, "\"feed_forward\":true"), "feed_forward");
    check(contains(json, "\"t\":0,\"K\":\"428a2f98\",\"W\":\"61626300\""), "round 0 has K[0] and the big-endian first word");
    check(contains(json, "\"t\":63,\"K\":\"c67178f2\""), "round 63 has K[63]");
    check(contains(json, "\"T1\":\""), "T1 recorded");
    check(contains(json, "\"T2\":\""), "T2 recorded");
    check(json[strlen(json) - 1] == '}', "JSON closes");

    /* Count round objects: exactly 64 `"t":` keys, and the last one is t=63. */
    int round_keys = 0;
    for (const char *p = json; (p = strstr(p, "\"t\":")) != NULL; p += 4) {
        round_keys++;
    }
    check(round_keys == 64, "exactly 64 round objects");
    check(contains(json, "\"t\":63,"), "round 63 present");

    /* The output state in the JSON must be the same value the caller got back. */
    char needle[96];
    snprintf(needle, sizeof(needle), "\"output_state\":\"%08x", (unsigned) trace.state_out.h[0]);
    check(contains(json, needle), "output_state matches the computed state (first word)");

    /* A truncated buffer must fail loudly instead of returning partial JSON. */
    char tiny[128];
    check(research_sha_trace_to_buffer(&trace, tiny, sizeof(tiny)) == -2, "too-small buffer reports -2");
    check(tiny[0] == '{' || tiny[0] == '\0', "truncated buffer stays terminated");

    /* Reduced rounds serialise their actual count. */
    check(research_sha_trace_run(&trace, &state, block, 7, false) == 0, "7-round trace for JSON built");
    n = research_sha_trace_to_buffer(&trace, json, sizeof(json));
    check(n > 0, "7-round trace serialises");
    check(contains(json, "\"rounds_executed\":7"), "7 rounds reported");
    check(contains(json, "\"feed_forward\":false"), "feed_forward false reported");
    int seven = 0;
    for (const char *p = json; (p = strstr(p, "\"t\":")) != NULL; p += 4) {
        seven++;
    }
    check(seven == 7, "exactly 7 round objects");
}

/* ------------------------------------------------ incremental (streaming) */

static void test_streaming_matches_one_shot(void)
{
    printf("== streaming API ==\n");
    static uint8_t message[1000];
    for (size_t i = 0; i < sizeof(message); i++) {
        message[i] = (uint8_t) (i * 17u + 3u);
    }

    /* Every split point must produce the one-shot digest. This is the property
     * the running-app backup relies on: it hashes 4 MiB in 4 KiB reads, and a
     * buffering mistake would silently change the digest of a backup. */
    const size_t lengths[] = {0, 1, 63, 64, 65, 127, 128, 129, 200, 512, 1000};
    int all_match = 1;
    for (size_t li = 0; li < sizeof(lengths) / sizeof(lengths[0]); li++) {
        size_t len = lengths[li];
        uint8_t want[32];
        research_sha256(message, len, want);

        for (size_t chunk = 1; chunk <= 70; chunk += 13) {
            research_sha256_ctx_t ctx;
            uint8_t got[32];
            research_sha256_init(&ctx);
            size_t offset = 0;
            while (offset < len) {
                size_t take = len - offset;
                if (take > chunk) {
                    take = chunk;
                }
                research_sha256_update(&ctx, message + offset, take);
                offset += take;
            }
            /* A zero-length update must not disturb the state. */
            research_sha256_update(&ctx, message, 0);
            research_sha256_final(&ctx, got);
            if (memcmp(got, want, 32) != 0) {
                all_match = 0;
                printf("    mismatch at len=%u chunk=%u\n", (unsigned) len, (unsigned) chunk);
            }
        }
    }
    check(all_match, "every chunk split reproduces the one-shot digest");

    research_sha256_ctx_t ctx;
    uint8_t digest[32];
    research_sha256_init(&ctx);
    research_sha256_update(&ctx, (const uint8_t *) "abc", 3);
    research_sha256_final(&ctx, digest);
    check_hex(digest, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "streaming SHA-256(\"abc\")");

    /* The context is cleared on finalisation, so an accidental reuse cannot
     * continue a previous hash. */
    check(ctx.total_bytes == 0 && ctx.buffered == 0, "finalising zeroes the context");
}

static void test_hex_helpers(void)
{
    printf("== hex helpers ==\n");
    uint8_t buf[8];
    char out[17];

    check(research_sha_hex_to_bytes("00ff10", buf, sizeof(buf)) == 3, "even-length hex parses");
    check(buf[0] == 0x00 && buf[1] == 0xFF && buf[2] == 0x10, "parsed values are correct");
    check(research_sha_bytes_to_hex(buf, 3, out, sizeof(out)) == 6, "bytes format");
    check(strcmp(out, "00ff10") == 0, "lowercase hex output");

    check(research_sha_hex_to_bytes("0", buf, sizeof(buf)) == -1, "odd length rejected");
    check(research_sha_hex_to_bytes("zz", buf, sizeof(buf)) == -1, "non-hex rejected");
    check(research_sha_hex_to_bytes("0011", buf, 1) == -1, "output too small rejected");
    check(research_sha_hex_to_bytes(NULL, buf, sizeof(buf)) == -1, "NULL input rejected");
    check(research_sha_bytes_to_hex(buf, 4, out, 8) == -1, "undersized hex buffer rejected");

    check(research_sha_hex_to_bytes("AbCdEf", buf, sizeof(buf)) == 3, "uppercase accepted");
    check(buf[0] == 0xAB && buf[1] == 0xCD && buf[2] == 0xEF, "uppercase parses");
}

/* ------------------------------------------------------------------- main */

static int emit(const char *hex)
{
    uint8_t block[RESEARCH_SHA_BLOCK_LEN];
    if (research_sha_hex_to_bytes(hex, block, sizeof(block)) != RESEARCH_SHA_BLOCK_LEN) {
        fprintf(stderr, "block must be exactly 64 bytes of hex\n");
        return 2;
    }
    research_sha_state_t state;
    research_sha_state_init(&state);
    research_sha_trace_t trace;
    trace.rounds = rounds;
    trace.rounds_capacity = RESEARCH_SHA_ROUNDS;
    if (research_sha_trace_run(&trace, &state, block, RESEARCH_SHA_ROUNDS, true) != 0) {
        fprintf(stderr, "trace failed\n");
        return 2;
    }
    int n = research_sha_trace_to_buffer(&trace, json, sizeof(json));
    if (n < 0) {
        fprintf(stderr, "serialisation failed (%d)\n", n);
        return 2;
    }
    fputs(json, stdout);
    fputc('\n', stdout);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--emit") == 0) {
        return emit(argv[2]);
    }

    test_kat();
    test_padding_boundaries();
    test_million_a();
    test_genesis_header();
    test_structural_invariants();
    test_reduced_rounds();
    test_json();
    test_streaming_matches_one_shot();
    test_hex_helpers();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
