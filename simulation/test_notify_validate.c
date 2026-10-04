/*
 * Host-side regression test for the mining.notify validator.
 *
 * The malformed fixtures are verbatim frames captured from the live device at
 * 192.168.178.66 (firmware v2.15.3-hardened) — see
 * evidence/192.168.178.66/snapshots/20261004-003232/bitaxe-logs.clean.txt and
 * docs/PLAN_BITAXE_BM1366_OPTIMIZATION.md, findings P0-1/P0-2.
 *
 * Build and run from this directory (no ESP-IDF required):
 *   clang -std=c11 -Wall -Wextra -Werror \
 *         -I ../components/stratum/include \
 *         ../components/stratum/notify_validate.c \
 *         test_notify_validate.c -o test_notify_validate
 *   ./test_notify_validate
 */

#include "notify_validate.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void check(bool cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL %s\n", what);
    }
}

/* ------------------------------------------------------------------ */
/* Fixture: a healthy notify observed on the device (job 7eb2)         */
/* ------------------------------------------------------------------ */
static const char *kPrevHash =
    "b9892e4ef50a39653f8571203a9bbc1caff64df83d7634bc0000000100000000";
static const char *kCoinbase1 =
    "01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff4e03d8ec00048c82c16a08";
static const char *kCoinbase2 =
    "2f7a706f6f6c2e63612f0381ceaf2f00fabe6d50c29fca174b8e9fbcfeeff14c7f5aabaef2c03fa4fb476e1b3c9c887cb3bb0780000000000000000000000001ca00062a010000001976a914ff7708c584d9233eeee7c63f312546d6e7c0df5988ac00000000";
static const char *kMerkle1 = "013a6db4d2bdb8818b89525db366002be22275b96950a2262155ebd350404704";
static const char *kMerkle2 = "93969c47a5c6c957956ef3040cd14555e6772a9b1fd30208adaf8768878359c0";
static const char *kMerkle3 = "b04b225d6ae9ab719dec4ac4c9c4742a2d703d8a752fee6cb2aef4f1915593b7";
static const char *kMerkle4 = "c9c7282c8346f1cc6ae7ef231801162d30b33c2372d7406cf233f098d074d764";
static const char *kMerkle5 = "c646bafece99fd102965094a6137ec1ec8c0997025e0fa2580dd84a6bc0f5269";

static stratum_v1_notify_view_t good_view(const char *const *merkle, size_t n_merkle)
{
    stratum_v1_notify_view_t v;
    memset(&v, 0, sizeof(v));
    v.job_id = "7eb2";
    v.prev_block_hash = kPrevHash;
    v.coinbase_1 = kCoinbase1;
    v.coinbase_2 = kCoinbase2;
    v.n_merkle_branches = n_merkle;
    v.merkle_branches = merkle;
    v.version = "20000000";
    v.nbits = "1901ad9e";
    v.ntime = "6ac18299";
    return v;
}

/* ------------------------------------------------------------------ */
/* Fixture: the malformed job 72b7 that was mined anyway             */
/*                                                                     */
/* Verbatim from the device log, params in wire order:                  */
/*   [0] job_id        "72b7"                (4)                        */
/*   [1] prev_block    64 hex chars           <- structurally fine       */
/*   [2] coinbase_1   112 hex chars (56 B)    <- split at an odd point    */
/*   [3] coinbase_2   138 hex chars (69 B)                                 */
/*   [4] merkle        []  -> 0 branches       <- every other job had 5   */
/*   [5] version      "00000006"              <- DGB algo bits 8..11 == 0*/
/*   [6] nbits        "1900c185"                                         */
/*   [7] ntime        "6ac179f3"                                         */
/*                                                                     */
/* Every field is well-formed hex, so field validation accepts it. The  */
/* anomaly is semantic (no DGB algorithm selector, zero merkle branches, */
/* implausible nBits), which is exactly why stratum_v1_task now gates the */
/* enqueue on coinbase_process_notification() instead of trusting parse.  */
/* ------------------------------------------------------------------ */
static const char *kBadPrevHash =
    "230210e0feb195870089f877e822a9077d809154ca768457637c17a27a804df3";
static const char *kBadCoinbase1 =
    "01000000f379c16a010000000000000000000000000000000000000000000000000000000000000000ffffffff2203bca50d04f379c16a08";
static const char *kBadCoinbase2 =
    "2f7a706f6f6c2e63612f0381ceaf2f000000000001a0b92b020000000023210224af11914c50d8a9f500091f56ab208a21997fe232d057d23ba9692f33c0b09aac00000000";

int main(void)
{
    char err[96];
    const char *merkle[] = { kMerkle1, kMerkle2, kMerkle3, kMerkle4, kMerkle5 };

    puts("== good notify accepted ==");
    stratum_v1_notify_view_t g = good_view(merkle, 5);
    check(stratum_v1_notify_validate(&g, err, sizeof(err)), "healthy notify must validate");
    if (failures) {
        printf("    reason: %s\n", err);
    }

    puts("== captured anomalous job 72b7 ==");
    stratum_v1_notify_view_t bad = good_view(NULL, 0);
    bad.job_id = "72b7";
    bad.prev_block_hash = kBadPrevHash;
    bad.coinbase_1 = kBadCoinbase1;
    bad.coinbase_2 = kBadCoinbase2;
    bad.version = "00000006";
    bad.nbits = "1900c185";
    bad.ntime = "6ac179f3";
    /* Structurally the hex is well-formed, so field validation accepts it —
     * the anomaly is semantic (no DGB algorithm selector in nVersion, zero
     * merkle branches, nBits implying difficulty ~8.7e4 instead of ~2.6e9).
     * coinbase_process_notification() rejects it, and stratum_v1_task now
     * gates the enqueue on that decoder. This test pins that division of
     * responsibility so neither half can silently regress. */
    check(stratum_v1_hex_is_valid(kBadCoinbase1, 56), "job 72b7 coinbase_1 is 56 well-formed bytes");
    check(!stratum_v1_hex_is_valid(kBadCoinbase1, 55), "job 72b7 coinbase_1 is not 55 bytes");
    check(stratum_v1_notify_validate(&bad, err, sizeof(err)),
          "job 72b7 passes field validation -> decoder is the gate");
    err[0] = '\0';
    bool bad_accepted = stratum_v1_notify_validate(&bad, err, sizeof(err));
    printf("    field validation for 72b7: %s%s%s\n",
           bad_accepted ? "accept" : "reject", bad_accepted ? "" : " -> ", err);
    check(stratum_v1_nbits_is_plausible("1900c185"),
          "0x1900c185 stays inside the compact-target floor (heuristic is Phase 2)");

    puts("== field-level rejections ==");
    stratum_v1_notify_view_t v;

    v = good_view(merkle, 5);
    v.prev_block_hash = "deadbeef";
    check(!stratum_v1_notify_validate(&v, err, sizeof(err)), "short prev_block_hash rejected");

    v = good_view(merkle, 5);
    v.prev_block_hash = "b9892e4ef50a39653f8571203a9bbc1caff64df83d7634bc000000010000000g";
    check(!stratum_v1_notify_validate(&v, err, sizeof(err)), "non-hex prev_block_hash rejected");

    v = good_view(merkle, 5);
    v.prev_block_hash = NULL;
    check(!stratum_v1_notify_validate(&v, err, sizeof(err)), "NULL prev_block_hash rejected");

    v = good_view(merkle, 5);
    {
        const char *short_merkle[] = { "013a6db4" };
        v.merkle_branches = short_merkle;
        v.n_merkle_branches = 1;
        check(!stratum_v1_notify_validate(&v, err, sizeof(err)), "short merkle branch rejected");
    }

    v = good_view(merkle, 5);
    v.n_merkle_branches = 5;
    v.merkle_branches = NULL;
    check(!stratum_v1_notify_validate(&v, err, sizeof(err)), "NULL merkle array with count>0 rejected");

    v = good_view(merkle, 5);
    v.job_id = "";
    check(!stratum_v1_notify_validate(&v, err, sizeof(err)), "empty job_id rejected");

    v = good_view(merkle, 5);
    v.job_id = "with\nnewline";
    check(!stratum_v1_notify_validate(&v, err, sizeof(err)), "control char in job_id rejected");

    v = good_view(merkle, 5);
    v.job_id = "0123456789012345678901234567890123"; /* 33 chars */
    check(!stratum_v1_notify_validate(&v, err, sizeof(err)), "over-long job_id rejected");

    v = good_view(merkle, 5);
    v.coinbase_2 = "abc";
    check(!stratum_v1_notify_validate(&v, err, sizeof(err)), "odd-length coinbase rejected");

    v = good_view(merkle, 5);
    v.coinbase_2 = NULL;
    check(!stratum_v1_notify_validate(&v, err, sizeof(err)), "NULL coinbase half rejected");

    v = good_view(merkle, 5);
    v.nbits = "00000000";
    check(!stratum_v1_notify_validate(&v, err, sizeof(err)), "zero nbits rejected");

    v = good_view(merkle, 5);
    v.nbits = "020000c1";
    check(!stratum_v1_notify_validate(&v, err, sizeof(err)), "nbits exponent < 0x03 rejected");

    v = good_view(merkle, 5);
    v.nbits = "3f000001";
    check(!stratum_v1_notify_validate(&v, err, sizeof(err)), "nbits exponent > 0x30 rejected");

    v = good_view(merkle, 5);
    v.nbits = NULL;
    check(!stratum_v1_notify_validate(&v, err, sizeof(err)), "NULL nbits rejected");

    v = good_view(merkle, 5);
    v.version = "zzzzzzzzz";
    check(!stratum_v1_notify_validate(&v, err, sizeof(err)), "non-hex version rejected");

    v = good_view(merkle, 5);
    v.ntime = "";
    check(!stratum_v1_notify_validate(&v, err, sizeof(err)), "empty ntime rejected");

    check(!stratum_v1_notify_validate(NULL, err, sizeof(err)), "NULL view rejected");

    puts("== zero-merkle coinbase-only block is legal ==");
    v = good_view(NULL, 0);
    check(stratum_v1_notify_validate(&v, err, sizeof(err)), "n_merkle_branches == 0 accepted");

    puts("== nbits plausibility ==");
    check(stratum_v1_nbits_is_plausible("1a1d4f9b"), "normal mainnet nbits accepted");
    check(stratum_v1_nbits_is_plausible("1901ad9e"), "device nbits accepted");
    check(!stratum_v1_nbits_is_plausible("00000000"), "zero rejected");
    check(!stratum_v1_nbits_is_plausible(NULL), "NULL rejected");

    puts("== hex helpers ==");
    check(stratum_v1_hex_is_valid(kMerkle1, 32), "64-char hex accepted");
    check(!stratum_v1_hex_is_valid(kMerkle1, 31), "wrong expected length rejected");
    check(!stratum_v1_hex_is_valid("", 32), "empty rejected");
    check(!stratum_v1_hex_is_valid(NULL, 32), "NULL rejected");
    check(stratum_v1_hex_num_is_valid("6ac179f3"), "8-char hex num accepted");
    check(!stratum_v1_hex_num_is_valid("6ac179f3a"), "9-char hex num rejected");

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
