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

/* Compact-target ("nBits") bounds.
 *
 * These mirror Bitcoin Core's acceptance of a compact target rather than being a
 * "looks safe" range picked here. Core decodes nBits in
 * arith_uint256::SetCompact and rejects the header in pow.cpp's DeriveTarget when
 * the decoded target is zero, when the sign bit is set, or when the value
 * overflows 256 bits:
 *
 *   size   = compact >> 24
 *   word   = compact & 0x007fffff
 *   target = word >> (8 * (3 - size))     when size <= 3
 *   target = word << (8 * (size - 3))     when size >  3
 *
 *   negative = word != 0 && (compact & 0x00800000)
 *   overflow = word != 0 && (size > 34
 *                            || (word > 0xff   && size > 33)
 *                            || (word > 0xffff && size > 32))
 *
 * The previous bounds here were a fixed exponent range of 0x03..0x30, which was
 * wrong at both ends. Above, it accepted exponents that Core classifies as an
 * overflow, so a job could be generated against a target no node would ever
 * accept. Below, it rejected size 1 and 2, which Core accepts whenever the
 * mantissa is wide enough to survive the shift. A single exponent bound cannot
 * express the rule anyway, because whether size 33 overflows depends on the
 * mantissa's width - so the validator implements Core's expression directly.
 *
 * Reference: github.com/bitcoin/bitcoin, src/arith_uint256.cpp (SetCompact) and
 * src/pow.cpp (DeriveTarget). The equivalence is differential-tested against an
 * independent reference implementation in simulation/test_nbits_reference.c.
 */
#define STRATUM_V1_NBITS_MIN_EXP 0x00
#define STRATUM_V1_NBITS_MAX_EXP 0x22

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
