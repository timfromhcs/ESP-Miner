#include "research_sha.h"

#include <stdio.h>
#include <string.h>

/* FIPS 180-4 section 4.2.2. */
static const uint32_t K[RESEARCH_SHA_ROUNDS] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static const uint32_t IV[RESEARCH_SHA_STATE_WORDS] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};

static uint32_t rotr(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

static void write_be32(uint32_t v, uint8_t *p)
{
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >> 8);
    p[3] = (uint8_t) v;
}

void research_sha_state_init(research_sha_state_t *state)
{
    if (state == NULL) {
        return;
    }
    memcpy(state->h, IV, sizeof(state->h));
}

void research_sha_expand(const uint8_t block[RESEARCH_SHA_BLOCK_LEN], uint32_t schedule[RESEARCH_SHA_ROUNDS])
{
    unsigned i;
    for (i = 0; i < 16; i++) {
        schedule[i] = read_be32(block + 4u * i);
    }
    for (i = 16; i < RESEARCH_SHA_ROUNDS; i++) {
        uint32_t s0 = rotr(schedule[i - 15], 7) ^ rotr(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3);
        uint32_t s1 = rotr(schedule[i - 2], 17) ^ rotr(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10);
        schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }
}

/* One compression. `work` is the working A..H vector; with `trace` non-NULL the
 * per-round values are recorded, which is the only difference between the hash
 * path and the research path. */
static void compress_block(const uint32_t schedule[RESEARCH_SHA_ROUNDS], uint32_t work[RESEARCH_SHA_STATE_WORDS],
                           unsigned rounds, research_sha_round_t *trace)
{
    uint32_t a = work[0], b = work[1], c = work[2], d = work[3];
    uint32_t e = work[4], f = work[5], g = work[6], h = work[7];
    unsigned t;

    for (t = 0; t < rounds; t++) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + ch + K[t] + schedule[t];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;

        if (trace != NULL) {
            research_sha_round_t *r = &trace[t];
            r->t = (uint8_t) t;
            r->k = K[t];
            r->w = schedule[t];
            r->in[0] = a;
            r->in[1] = b;
            r->in[2] = c;
            r->in[3] = d;
            r->in[4] = e;
            r->in[5] = f;
            r->in[6] = g;
            r->in[7] = h;
            r->t1 = t1;
            r->t2 = t2;
        }

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;

        if (trace != NULL) {
            research_sha_round_t *r = &trace[t];
            r->out[0] = a;
            r->out[1] = b;
            r->out[2] = c;
            r->out[3] = d;
            r->out[4] = e;
            r->out[5] = f;
            r->out[6] = g;
            r->out[7] = h;
        }
    }

    work[0] = a;
    work[1] = b;
    work[2] = c;
    work[3] = d;
    work[4] = e;
    work[5] = f;
    work[6] = g;
    work[7] = h;
}

void research_sha_compress(research_sha_state_t *state, const uint8_t block[RESEARCH_SHA_BLOCK_LEN])
{
    uint32_t schedule[RESEARCH_SHA_ROUNDS];
    uint32_t work[RESEARCH_SHA_STATE_WORDS];
    unsigned i;

    if (state == NULL || block == NULL) {
        return;
    }
    research_sha_expand(block, schedule);
    memcpy(work, state->h, sizeof(work));
    compress_block(schedule, work, RESEARCH_SHA_ROUNDS, NULL);
    for (i = 0; i < RESEARCH_SHA_STATE_WORDS; i++) {
        state->h[i] += work[i];
    }
}

void research_sha256(const uint8_t *message, size_t len, uint8_t out[RESEARCH_SHA_DIGEST_LEN])
{
    research_sha_state_t state;
    uint8_t tail[RESEARCH_SHA_BLOCK_LEN * 2];
    size_t offset = 0;
    size_t rest;
    size_t tail_len;
    uint64_t bits;
    unsigned i;

    if (out == NULL) {
        return;
    }
    if (message == NULL) {
        len = 0;
    }

    research_sha_state_init(&state);
    while (len - offset >= RESEARCH_SHA_BLOCK_LEN) {
        research_sha_compress(&state, message + offset);
        offset += RESEARCH_SHA_BLOCK_LEN;
    }

    rest = len - offset;
    memset(tail, 0, sizeof(tail));
    if (rest > 0) {
        memcpy(tail, message + offset, rest);
    }
    tail[rest] = 0x80;
    /* 1 byte for 0x80 plus 8 bytes of length must fit in the final block. */
    tail_len = (rest < RESEARCH_SHA_BLOCK_LEN - 8) ? RESEARCH_SHA_BLOCK_LEN : RESEARCH_SHA_BLOCK_LEN * 2;

    /* Callers on this device hash bounded inputs (an 80-byte header, a client
     * block, a firmware digest), so the 64-bit length never overflows here. */
    bits = (uint64_t) len * 8u;
    for (i = 0; i < 8; i++) {
        tail[tail_len - 1 - i] = (uint8_t) (bits >> (8u * i));
    }

    research_sha_compress(&state, tail);
    if (tail_len == RESEARCH_SHA_BLOCK_LEN * 2) {
        research_sha_compress(&state, tail + RESEARCH_SHA_BLOCK_LEN);
    }

    for (i = 0; i < RESEARCH_SHA_STATE_WORDS; i++) {
        write_be32(state.h[i], out + 4u * i);
    }
}

void research_sha256d(const uint8_t *message, size_t len, uint8_t out[RESEARCH_SHA_DIGEST_LEN])
{
    uint8_t inner[RESEARCH_SHA_DIGEST_LEN];
    if (out == NULL) {
        return;
    }
    research_sha256(message, len, inner);
    research_sha256(inner, sizeof(inner), out);
}

int research_sha_trace_run(research_sha_trace_t *trace, const research_sha_state_t *state_in, const uint8_t block[RESEARCH_SHA_BLOCK_LEN],
                           unsigned rounds, bool feed_forward)
{
    uint32_t work[RESEARCH_SHA_STATE_WORDS];
    unsigned i;

    if (trace == NULL || state_in == NULL || block == NULL || rounds > RESEARCH_SHA_ROUNDS) {
        return -1;
    }
    if (rounds > 0 && (trace->rounds == NULL || trace->rounds_capacity < rounds)) {
        return -2;
    }

    memcpy(trace->block, block, RESEARCH_SHA_BLOCK_LEN);
    trace->state_in = *state_in;
    trace->feed_forward = feed_forward;
    trace->rounds_executed = rounds;
    research_sha_expand(block, trace->schedule);

    memcpy(work, state_in->h, sizeof(work));
    compress_block(trace->schedule, work, rounds, rounds > 0 ? trace->rounds : NULL);

    for (i = 0; i < RESEARCH_SHA_STATE_WORDS; i++) {
        trace->state_out.h[i] = feed_forward ? state_in->h[i] + work[i] : work[i];
    }
    return 0;
}

/* --- JSON emission ------------------------------------------------------- */

static int emit_raw(research_sha_write_fn write, void *ctx, const char *s)
{
    return write(ctx, s, strlen(s));
}

static void hex32(char out[9], uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 8; i++) {
        out[7 - i] = digits[v & 0xFu];
        v >>= 4;
    }
    out[8] = '\0';
}

static int emit_state_expect(research_sha_write_fn write, void *ctx, const char *name, const uint32_t h[RESEARCH_SHA_STATE_WORDS])
{
    char buf[176];
    char w[RESEARCH_SHA_STATE_WORDS][9];
    int i;
    for (i = 0; i < RESEARCH_SHA_STATE_WORDS; i++) {
        hex32(w[i], h[i]);
    }
    int n = snprintf(buf, sizeof(buf),
                     "\"%s\":{\"a\":\"%s\",\"b\":\"%s\",\"c\":\"%s\",\"d\":\"%s\",\"e\":\"%s\",\"f\":\"%s\",\"g\":\"%s\",\"h\":\"%s\"}",
                     name, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    if (n <= 0 || (size_t) n >= sizeof(buf)) {
        return -1;
    }
    return write(ctx, buf, (size_t) n);
}

static int emit_state_hex(research_sha_write_fn write, void *ctx, const char *name, const uint32_t h[RESEARCH_SHA_STATE_WORDS])
{
    char buf[96];
    char hex[RESEARCH_SHA_STATE_WORDS * 8 + 1];
    int i;
    for (i = 0; i < RESEARCH_SHA_STATE_WORDS; i++) {
        hex32(hex + 8 * i, h[i]);
    }
    int n = snprintf(buf, sizeof(buf), "\"%s\":\"%s\"", name, hex);
    if (n <= 0 || (size_t) n >= sizeof(buf)) {
        return -1;
    }
    return write(ctx, buf, (size_t) n);
}

int research_sha_trace_write_json(const research_sha_trace_t *trace, research_sha_write_fn write, void *ctx)
{
    if (trace == NULL || write == NULL) {
        return -1;
    }
    if (trace->rounds_executed > RESEARCH_SHA_ROUNDS) {
        return -1;
    }
    if (trace->rounds_executed > 0 && trace->rounds == NULL) {
        return -1;
    }

    int rc = emit_raw(write, ctx, "{\"schema\":\"hcs.sha.trace.v1\",\"source\":\"SOFTWARE_REPLAY\",\"block\":\"");
    if (rc != 0) {
        return rc;
    }

    for (unsigned i = 0; i < RESEARCH_SHA_BLOCK_LEN; i++) {
        static const char digits[] = "0123456789abcdef";
        char pair[2];
        pair[0] = digits[trace->block[i] >> 4];
        pair[1] = digits[trace->block[i] & 0xFu];
        rc = write(ctx, pair, 2);
        if (rc != 0) {
            return rc;
        }
    }

    rc = emit_raw(write, ctx, "\",");
    if (rc == 0) {
        rc = emit_state_hex(write, ctx, "input_state", trace->state_in.h);
    }
    if (rc == 0) {
        rc = emit_raw(write, ctx, ",\"schedule\":[");
    }
    for (unsigned i = 0; rc == 0 && i < RESEARCH_SHA_ROUNDS; i++) {
        char buf[12];
        char w[9];
        hex32(w, trace->schedule[i]);
        int n = snprintf(buf, sizeof(buf), "%s\"%s\"", i == 0 ? "" : ",", w);
        if (n <= 0 || (size_t) n >= sizeof(buf)) {
            return -1;
        }
        rc = write(ctx, buf, (size_t) n);
    }
    if (rc == 0) {
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "],\"rounds_executed\":%u,\"feed_forward\":%s,\"rounds\":[", (unsigned) trace->rounds_executed,
                         trace->feed_forward ? "true" : "false");
        if (n <= 0 || (size_t) n >= sizeof(buf)) {
            return -1;
        }
        rc = write(ctx, buf, (size_t) n);
    }

    for (size_t t = 0; rc == 0 && t < trace->rounds_executed; t++) {
        const research_sha_round_t *r = &trace->rounds[t];
        char k[9], w[9], t1[9], t2[9];
        hex32(k, r->k);
        hex32(w, r->w);
        hex32(t1, r->t1);
        hex32(t2, r->t2);
        char head[96];
        int n = snprintf(head, sizeof(head), "%s{\"t\":%u,\"K\":\"%s\",\"W\":\"%s\",", t == 0 ? "" : ",", (unsigned) r->t, k, w);
        if (n <= 0 || (size_t) n >= sizeof(head)) {
            return -1;
        }
        rc = write(ctx, head, (size_t) n);
        if (rc == 0) {
            rc = emit_state_expect(write, ctx, "in", r->in);
        }
        if (rc == 0) {
            char mid[64];
            int m = snprintf(mid, sizeof(mid), ",\"T1\":\"%s\",\"T2\":\"%s\",", t1, t2);
            if (m <= 0 || (size_t) m >= sizeof(mid)) {
                return -1;
            }
            rc = write(ctx, mid, (size_t) m);
        }
        if (rc == 0) {
            rc = emit_state_expect(write, ctx, "out", r->out);
        }
        if (rc == 0) {
            rc = emit_raw(write, ctx, "}");
        }
    }

    if (rc == 0) {
        rc = emit_raw(write, ctx, "],");
    }
    if (rc == 0) {
        rc = emit_state_hex(write, ctx, "output_state", trace->state_out.h);
    }
    if (rc == 0) {
        rc = emit_raw(write, ctx, "}");
    }
    return rc;
}

typedef struct {
    char *out;
    size_t capacity;
    size_t used;
    bool overflow;
} buffer_writer_t;

static int buffer_write(void *ctx, const char *data, size_t len)
{
    buffer_writer_t *w = (buffer_writer_t *) ctx;
    if (w->used + len + 1 > w->capacity) {
        w->overflow = true;
        return -1;
    }
    memcpy(w->out + w->used, data, len);
    w->used += len;
    w->out[w->used] = '\0';
    return 0;
}

int research_sha_trace_to_buffer(const research_sha_trace_t *trace, char *out, size_t out_len)
{
    buffer_writer_t writer;

    if (trace == NULL || out == NULL || out_len == 0) {
        return -1;
    }
    out[0] = '\0';
    writer.out = out;
    writer.capacity = out_len;
    writer.used = 0;
    writer.overflow = false;

    int rc = research_sha_trace_write_json(trace, buffer_write, &writer);
    if (rc != 0)
        return writer.overflow ? -2 : rc;
    return (int) writer.used;
}

/* --- hex helpers --------------------------------------------------------- */

int research_sha_bytes_to_hex(const uint8_t *in, size_t len, char *out, size_t out_len)
{
    static const char digits[] = "0123456789abcdef";
    if (in == NULL || out == NULL || out_len < len * 2 + 1) {
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0xFu];
    }
    out[len * 2] = '\0';
    return (int) (len * 2);
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int research_sha_hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    size_t n;
    if (hex == NULL || out == NULL) {
        return -1;
    }
    for (n = 0; hex[2 * n] != '\0' && hex[2 * n + 1] != '\0'; n++) {
        if (n >= out_len) {
            return -1;
        }
        int hi = hex_nibble(hex[2 * n]);
        int lo = hex_nibble(hex[2 * n + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[n] = (uint8_t) ((hi << 4) | lo);
    }
    /* Odd length or a stray terminator inside a pair is malformed. */
    if (hex[2 * n] != '\0') {
        return -1;
    }
    return (int) n;
}
