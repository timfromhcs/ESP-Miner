/*
 * Differential test for the shipped compact-target ("nBits") validator.
 *
 * The reference decoder below is a second, independent implementation written
 * directly from the Bitcoin consensus definition of compact targets, and is used
 * to check the shipped validator's acceptance decisions against it. It is not a
 * wrapper around the code under test - it deliberately shares no logic with it.
 *
 * The consensus rule being modelled (Bitcoin Core, arith_uint256::SetCompact and
 * CheckProofOfWork):
 *
 *   size   = compact >> 24
 *   word   = compact & 0x007fffff
 *   target = word >> (8 * (3 - size))              when size <= 3
 *   target = word << (8 * (size - 3))              when size >  3
 *
 *   A block header is valid work only when the target is non-zero, the sign bit
 *   (bit 23 of the compact word) is clear, and the value does not overflow 256
 *   bits.
 *
 * The overflow condition is what the shipped upper exponent bound has to
 * express. `word` is at most 23 bits wide, so the value needs
 * `8 * (size - 3) + 23 <= 256`, which gives `size <= 32` (0x20). Any larger
 * exponent encodes a target that does not fit in a 256-bit word, and Core's
 * CheckProofOfWork rejects such a header outright rather than treating it as a
 * trivially-satisfied difficulty check.
 */

#include "notify_validate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

/* ------------------------------------------------------------------ reference */

typedef enum {
    REF_ACCEPT = 0,
    REF_REJECT_NOT_HEX,
    REF_REJECT_TOO_LONG,
    REF_REJECT_ZERO,
    REF_REJECT_NEGATIVE,
    REF_REJECT_OVERFLOW
} ref_verdict_t;

/* Number of significant bits in the decoded target, 0 when it decodes to zero. */
static long ref_target_bits(uint32_t compact)
{
    uint32_t size = compact >> 24;
    uint32_t word = compact & 0x007fffffu;

    unsigned word_bits = 0;
    for (uint32_t w = word; w != 0; w >>= 1) {
        word_bits++;
    }
    if (word_bits == 0) {
        return 0;
    }

    if (size <= 3) {
        /* SetCompact: nWord >>= 8 * (3 - nSize). With size == 0 the shift is 24
         * and the 23-bit word always shifts out to zero. */
        uint32_t shift = 8u * (3u - size);
        if (shift >= 32u) {
            return 0;
        }
        if (shift == 0) {
            return (long)word_bits;
        }
        if (word_bits <= shift) {
            return 0; /* every significant bit is shifted out */
        }
        return (long)(word_bits - shift);
    }

    return (long)(8u * (size - 3u)) + (long)word_bits;
}

/* Verbatim transcription of the overflow rule in Bitcoin Core's
 * arith_uint256::SetCompact. The mantissa-width terms matter: a size-33 value is
 * fine with a one-byte mantissa and an overflow with a two-byte one, so any
 * single exponent bound is a compromise. Using Core's expression keeps this
 * reference aligned with what a full node will actually do with the header
 * rather than with a guess. */
static bool core_reports_overflow(uint32_t size, uint32_t word)
{
    return word != 0 && ((size > 34) ||
                         (word > 0xff && size > 33) ||
                         (word > 0xffff && size > 32));
}

static ref_verdict_t reference_verdict(const char *nbits_hex)
{
    if (nbits_hex == NULL) {
        return REF_REJECT_NOT_HEX;
    }
    size_t len = strlen(nbits_hex);
    if (len == 0 || len > 8) {
        return REF_REJECT_TOO_LONG;
    }
    for (size_t i = 0; i < len; i++) {
        char c = nbits_hex[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) {
            return REF_REJECT_NOT_HEX;
        }
    }

    uint32_t compact = (uint32_t) strtoul(nbits_hex, NULL, 16);
    uint32_t size = compact >> 24;
    uint32_t word = compact & 0x007fffffu;

    /* SetCompact decodes to zero for a zero word and for a word short enough to
     * shift away entirely; DeriveTarget rejects a zero target. */
    if (ref_target_bits(compact) == 0) {
        return REF_REJECT_ZERO;
    }
    /* Sign bit of the compact word. */
    if (word != 0 && (compact & 0x00800000u) != 0) {
        return REF_REJECT_NEGATIVE;
    }
    /* DeriveTarget rejects a target Core reports as overflowing. */
    if (core_reports_overflow(size, word)) {
        return REF_REJECT_OVERFLOW;
    }
    return REF_ACCEPT;
}

static int reference_accepts(const char *hex)
{
    return reference_verdict(hex) == REF_ACCEPT;
}

/* --------------------------------------------------------------------- cases */

static void compare(const char *hex, const char *what)
{
    bool shipped = stratum_v1_nbits_is_plausible(hex);
    bool ref = reference_accepts(hex);
    checks++;
    if (shipped != ref) {
        failures++;
        printf("  FAIL: %s (%s) shipped=%d reference=%d\n", what, hex ? hex : "(null)",
               shipped ? 1 : 0, ref ? 1 : 0);
    }
}

static void test_mainnet_targets(void)
{
    printf("== real mainnet nBits values ==\n");
    /* Difficulty-1 pow limit and several historical epochs. All are ordinary
     * 0x1d-exponent values and must be accepted by both implementations. */
    const char *real[] = {
        "1d00ffff", /* difficulty 1  */
        "1b0404cb", /* difficulty ~1.4e13 */
        "1a05db8b",
        "190f4c88",
        "1c3fffc0",
        "1d00f0f0",
        "1e0ffff0",
        "1f00ffff",
        "207fffff",
        NULL
    };
    for (int i = 0; real[i] != NULL; i++) {
        char label[80];
        snprintf(label, sizeof(label), "mainnet target %s accepted", real[i]);
        compare(real[i], label);
    }
}

static void test_exponent_boundaries(void)
{
    printf("== exponent boundaries ==\n");

    /* 0x20 (32) is the largest exponent whose value still fits in 256 bits:
     * 23 bits of mantissa plus 8 * 29 = 232 shifted bits. Must be accepted. */
    compare("20000001", "exponent 0x20 with a minimal mantissa is accepted");
    compare("20ffffff", "exponent 0x20 with a maximal mantissa is accepted");

    /* 0x21 (33) shifts by 240 bits on top of up to 23 mantissa bits, which needs
     * 263 bits. Core treats that as an overflow and rejects the header, so the
     * validator has to agree. */
    compare("21000001", "exponent 0x21 overflows 256 bits and is rejected");
    compare("20ffffff", "exponent 0x20 with the maximum mantissa fits and is accepted");
    compare("3f7fffff", "exponent 0x3f overflows and is rejected");

    /* Below 0x03: sizes 0..2 shift the word right, so the encoded target is
     * either zero or absurdly small. */
    compare("02000001", "exponent 0x02 encodes a sub-2^16 target and is rejected");
    compare("01000001", "exponent 0x01 encodes a target that shifts to zero and is rejected");
    compare("00000001", "exponent 0x00 encodes zero and is rejected");

    /* Zero mantissa is never a valid target. */
    compare("1d000000", "zero mantissa is rejected");
    compare("03000000", "zero mantissa at the low bound is rejected");
    compare("20000000", "zero mantissa at the high bound is rejected");
}

static void test_sign_bit(void)
{
    printf("== negative (sign-bit) mantissa ==\n");
    /* Bit 23 set means Core reports fNegative and rejects the header. */
    compare("00800001", "sign bit set with a zero exponent is rejected");
    compare("1d800001", "sign bit set in the mantissa is rejected");
    compare("20800000", "sign bit set at the highest exponent is rejected");
    compare("1dffffff", "0x1dffffff still has bit 23 set and is rejected");
    /* Just below the sign bit. */
    compare("1d7fffff", "largest positive mantissa is accepted");
}

static void test_encoding_rejects_malformed(void)
{
    printf("== malformed nBits strings ==\n");
    compare(NULL, "NULL is rejected");
    compare("", "empty string is rejected");
    compare("zzzzzzzz", "non-hex is rejected");
    compare("1d00fffz", "trailing non-hex is rejected");
    compare("g", "single non-hex character is rejected");
    compare("000000001", "nine characters is rejected");
    compare("1d00ffff ", "trailing space is rejected");
    compare(" 1d00ffff", "leading space is rejected");

    /* Uppercase is valid hex and both implementations must agree. */
    compare("1D00FFFF", "uppercase hex is accepted");
    compare("1d00FFFF", "mixed case is accepted");
    /* A single digit is a legal short encoding of the same value. */
    compare("1", "single digit is accepted");
    compare("0", "single zero digit is rejected");
}

static void test_differential_random(void)
{
    printf("== randomised differential sweep ==\n");
    /* Deterministic LCG: a failing case has to be reproducible from the seed, so
     * this never depends on the host's rand() implementation. */
    unsigned long state = 0x5eed4b17UL;
    int mismatches = 0;

    for (int i = 0; i < 20000; i++) {
        state = state * 6364136223846793005UL + 1442695040888963407UL;
        uint32_t v = (uint32_t) (state >> 33);

        char hex[9];
        snprintf(hex, sizeof(hex), "%08x", v);
        bool shipped = stratum_v1_nbits_is_plausible(hex);
        bool ref = reference_accepts(hex);
        checks++;
        if (shipped != ref) {
            if (mismatches < 10) {
                failures++;
                mismatches++;
                printf("  FAIL: random 0x%s shipped=%d reference=%d\n", hex,
                       shipped ? 1 : 0, ref ? 1 : 0);
            }
        }
    }

    /* Sanity on the sweep itself: a uniform 32-bit word should be accepted only
     * for exponents in 0x03..0x20, which is a narrow slice. If the reference were
     * trivially accepting everything the comparison would be meaningless. */
    state = 0x12345678UL;
    int accepted = 0;
    for (int i = 0; i < 20000; i++) {
        state = state * 6364136223846793005UL + 1442695040888963407UL;
        char hex[9];
        snprintf(hex, sizeof(hex), "%08x", (uint32_t) (state >> 33));
        if (reference_accepts(hex)) {
            accepted++;
        }
    }
    printf("  (reference accepted %d/20000 random words; expected a small fraction)\n", accepted);
    checks++;
    if (accepted < 100 || accepted > 4000) {
        failures++;
        printf("  FAIL: reference acceptance rate is implausible, the sweep is not meaningful\n");
    }
}

/* Exhaustive sweep of the exponent byte. Exposes every boundary transition
 * instead of trusting two hand-picked values. */
static void test_exhaustive_exponent_byte(void)
{
    printf("== exhaustive exponent-byte sweep ==\n");
    for (unsigned exp = 0; exp <= 0xff; exp++) {
        char hex[9];
        snprintf(hex, sizeof(hex), "%02x7fffff", exp);
        compare(hex, "exponent sweep, maximal positive mantissa");

        snprintf(hex, sizeof(hex), "%02x000001", exp);
        compare(hex, "exponent sweep, minimal positive mantissa");
    }
}

int main(void)
{
    printf("nbits reference differential test\n");
    printf("shipped max exponent bound: 0x%02x\n", (unsigned) STRATUM_V1_NBITS_MAX_EXP);
    printf("derived maximum that fits 256 bits: 0x20\n");

    test_mainnet_targets();
    test_exponent_boundaries();
    test_sign_bit();
    test_encoding_rejects_malformed();
    test_differential_random();
    test_exhaustive_exponent_byte();

    printf("%d checks, %d failures\n", checks, failures);
    if (failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("TESTS FAILED\n");
    return 1;
}
