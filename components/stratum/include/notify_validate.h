#ifndef NOTIFY_VALIDATE_H
#define NOTIFY_VALIDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Dependency-free validation of a Stratum V1 `mining.notify`.
 *
 * Rationale (see docs/PLAN_BITAXE_BM1366_OPTIMIZATION.md, finding P0-1):
 * a malformed `mining.notify` used to be enqueued and handed to the ASIC
 * anyway, and short/malformed hex strings made `hex2bin()` leave parts of the
 * freshly `malloc()`ed merkle array uninitialised, which was then hashed by
 * the chip. Validating here keeps the parser honest and lets the caller drop
 * the job before any work is generated.
 */

#define STRATUM_V1_HASH_HEX_LEN   64   /* 32-byte hash, hex encoded          */
#define STRATUM_V1_MAX_COINBASE_HEX 8192
#define STRATUM_V1_MAX_JOB_ID_LEN 32

/* Compact-target ("nBits") exponent bounds. A target outside 2^0..2^256 is
 * definitionally impossible, so a compliant pool can never send it. */
#define STRATUM_V1_NBITS_MIN_EXP 0x03
#define STRATUM_V1_NBITS_MAX_EXP 0x30

typedef struct {
    const char *job_id;
    const char *prev_block_hash;  /* must be exactly 64 hex chars          */
    const char *coinbase_1;       /* both halves concatenated must be hex  */
    const char *coinbase_2;       /* and have an even total length         */
    size_t      n_merkle_branches;
    const char *const *merkle_branches; /* each must be exactly 64 hex chars */
    const char *version;          /* 1..8 hex chars                        */
    const char *nbits;            /* 1..8 hex chars                        */
    const char *ntime;            /* 1..8 hex chars                        */
} stratum_v1_notify_view_t;

/** True when `s` is non-NULL, exactly `expect_bytes * 2` characters long and
 *  every character is a hex digit. */
bool stratum_v1_hex_is_valid(const char *s, size_t expect_bytes);

/** True when `s` is non-NULL, 1..8 hex characters (Stratum numeric fields). */
bool stratum_v1_hex_num_is_valid(const char *s);

/** True when the compact target encoded in `nbits_hex` denotes a value in the
 *  representable range. Rejects exponent < 0x03 or > 0x30, and a zero target. */
bool stratum_v1_nbits_is_plausible(const char *nbits_hex);

/**
 * @brief Validate every field of a `mining.notify` before it is enqueued.
 *
 * @param v     field view; individual pointers may be NULL, which is reported
 *              through `err` rather than dereferenced.
 * @param err   optional buffer receiving a human-readable reason (may be NULL).
 * @param errlen size of `err`.
 *
 * @return true when the notification is safe to turn into chip work.
 */
bool stratum_v1_notify_validate(const stratum_v1_notify_view_t *v, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* NOTIFY_VALIDATE_H */
