#include "notify_validate.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* strnlen() is POSIX, not ISO C; bounded_len() keeps this module buildable with
 * the MSVC/Windows toolchains used for the host-side unit tests. */
static size_t bounded_len(const char *s, size_t max)
{
    if (s == NULL) {
        return 0;
    }
    size_t n = 0;
    while (n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool all_hex(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (hex_val(s[i]) < 0) {
            return false;
        }
    }
    return true;
}

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    if (err == NULL || errlen == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

bool stratum_v1_hex_is_valid(const char *s, size_t expect_bytes)
{
    if (s == NULL) {
        return false;
    }
    size_t want = expect_bytes * 2;
    size_t len = bounded_len(s, want + 1);
    if (len != want) {
        return false;
    }
    return all_hex(s, len);
}

bool stratum_v1_hex_num_is_valid(const char *s)
{
    if (s == NULL) {
        return false;
    }
    size_t len = bounded_len(s, 9);
    if (len == 0 || len > 8) {
        return false;
    }
    return all_hex(s, len);
}

bool stratum_v1_nbits_is_plausible(const char *nbits_hex)
{
    if (!stratum_v1_hex_num_is_valid(nbits_hex)) {
        return false;
    }
    uint32_t nbits = (uint32_t) strtoul(nbits_hex, NULL, 16);
    uint32_t exponent = nbits >> 24;
    uint32_t mantissa = nbits & 0x00FFFFFFu;

    if (mantissa == 0) {
        return false; /* target of zero can never be met */
    }
    if (exponent < STRATUM_V1_NBITS_MIN_EXP || exponent > STRATUM_V1_NBITS_MAX_EXP) {
        return false;
    }
    /* A negative mantissa (high bit of the 24-bit field set) is sign-extended
     * by the compact encoding; reject it instead of hashing toward a huge
     * target. */
    if (mantissa & 0x00800000u) {
        return false;
    }
    return true;
}

bool stratum_v1_notify_validate(const stratum_v1_notify_view_t *v, char *err, size_t errlen)
{
    if (v == NULL) {
        set_err(err, errlen, "notify view is NULL");
        return false;
    }

    /* --- job id ---------------------------------------------------------- */
    if (v->job_id == NULL) {
        set_err(err, errlen, "job_id missing");
        return false;
    }
    size_t job_id_len = bounded_len(v->job_id, STRATUM_V1_MAX_JOB_ID_LEN + 1);
    if (job_id_len == 0 || job_id_len > STRATUM_V1_MAX_JOB_ID_LEN) {
        set_err(err, errlen, "job_id length %u out of range 1..%d",
                (unsigned) job_id_len, STRATUM_V1_MAX_JOB_ID_LEN);
        return false;
    }
    for (size_t i = 0; i < job_id_len; i++) {
        unsigned char c = (unsigned char) v->job_id[i];
        if (c < 0x21 || c > 0x7e) {
            set_err(err, errlen, "job_id contains non-printable byte 0x%02x", c);
            return false;
        }
    }

    /* --- prev block hash: exactly 32 bytes -------------------------------- */
    if (!stratum_v1_hex_is_valid(v->prev_block_hash, 32)) {
        set_err(err, errlen, "prev_block_hash is not 32 bytes of hex");
        return false;
    }

    /* --- coinbase halves ------------------------------------------------- */
    if (v->coinbase_1 == NULL || v->coinbase_2 == NULL) {
        set_err(err, errlen, "coinbase half missing");
        return false;
    }
    size_t cb1 = bounded_len(v->coinbase_1, STRATUM_V1_MAX_COINBASE_HEX + 1);
    size_t cb2 = bounded_len(v->coinbase_2, STRATUM_V1_MAX_COINBASE_HEX + 1);
    if (cb1 == 0 || cb2 == 0) {
        set_err(err, errlen, "coinbase half empty (%u/%u)", (unsigned) cb1, (unsigned) cb2);
        return false;
    }
    if (cb1 + cb2 > STRATUM_V1_MAX_COINBASE_HEX) {
        set_err(err, errlen, "coinbase %u bytes exceeds limit %d",
                (unsigned) (cb1 + cb2), STRATUM_V1_MAX_COINBASE_HEX);
        return false;
    }
    if (((cb1 + cb2) & 1u) != 0) {
        set_err(err, errlen, "coinbase length %u is odd", (unsigned) (cb1 + cb2));
        return false;
    }
    if (!all_hex(v->coinbase_1, cb1) || !all_hex(v->coinbase_2, cb2)) {
        set_err(err, errlen, "coinbase contains non-hex characters");
        return false;
    }

    /* --- merkle branches ------------------------------------------------- */
    for (size_t i = 0; i < v->n_merkle_branches; i++) {
        if (v->merkle_branches == NULL) {
            set_err(err, errlen, "merkle branch array missing but count is %u",
                    (unsigned) v->n_merkle_branches);
            return false;
        }
        if (!stratum_v1_hex_is_valid(v->merkle_branches[i], 32)) {
            set_err(err, errlen, "merkle branch %u is not 32 bytes of hex", (unsigned) i);
            return false;
        }
    }

    /* --- numeric fields --------------------------------------------------- */
    if (!stratum_v1_hex_num_is_valid(v->version)) {
        set_err(err, errlen, "version is not 1..8 hex chars");
        return false;
    }
    if (!stratum_v1_nbits_is_plausible(v->nbits)) {
        set_err(err, errlen, "nbits '%s' is not a plausible compact target",
                v->nbits ? v->nbits : "(null)");
        return false;
    }
    if (!stratum_v1_hex_num_is_valid(v->ntime)) {
        set_err(err, errlen, "ntime is not 1..8 hex chars");
        return false;
    }

    return true;
}
