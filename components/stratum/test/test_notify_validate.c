/* Unity tests for the Stratum V1 mining.notify validator.
 *
 * Runs in CI via .github/workflows/unittest.yml (QEMU, test-ci project).
 * The host-only mirror of these checks lives in simulation/test_notify_validate.c
 * and is what developers run locally, because it needs neither ESP-IDF nor
 * hardware.
 */

#include <string.h>

#include "unity.h"

#include "notify_validate.h"

/* Verbatim from the live device at 192.168.178.66, job 7eb2. */
static const char *kJobId = "7eb2";
static const char *kPrevHash =
    "b9892e4ef50a39653f8571203a9bbc1caff64df83d7634bc0000000100000000";
static const char *kCoinbase1 =
    "01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff4e03d8ec00048c82c16a08";
static const char *kCoinbase2 =
    "2f7a706f6f6c2e63612f0381ceaf2f00fabe6d50c29fca174b8e9fbcfeeff14c7f5aabaef2c03fa4fb476e1b3c9c887cb3bb0780000000000000000000000001ca00062a010000001976a914ff7708c584d9233eeee7c63f312546d6e7c0df5988ac00000000";
static const char *kMerkle[5] = {
    "013a6db4d2bdb8818b89525db366002be22275b96950a2262155ebd350404704",
    "93969c47a5c6c957956ef3040cd14555e6772a9b1fd30208adaf8768878359c0",
    "b04b225d6ae9ab719dec4ac4c9c4742a2d703d8a752fee6cb2aef4f1915593b7",
    "c9c7282c8346f1cc6ae7ef231801162d30b33c2372d7406cf233f098d074d764",
    "c646bafece99fd102965094a6137ec1ec8c0997025e0fa2580dd84a6bc0f5269",
};

static stratum_v1_notify_view_t good_view(void)
{
    stratum_v1_notify_view_t v;
    memset(&v, 0, sizeof(v));
    v.job_id = kJobId;
    v.prev_block_hash = kPrevHash;
    v.coinbase_1 = kCoinbase1;
    v.coinbase_2 = kCoinbase2;
    v.n_merkle_branches = 5;
    v.merkle_branches = kMerkle;
    v.version = "20000000";
    v.nbits = "1901ad9e";
    v.ntime = "6ac18299";
    return v;
}

static void assert_rejected(stratum_v1_notify_view_t v, const char *what)
{
    char err[96] = {0};
    TEST_ASSERT_FALSE_MESSAGE(stratum_v1_notify_validate(&v, err, sizeof(err)), what);
}

void test_notify_validate_accepts_healthy_notify(void)
{
    stratum_v1_notify_view_t v = good_view();
    char err[96] = {0};
    TEST_ASSERT_TRUE_MESSAGE(stratum_v1_notify_validate(&v, err, sizeof(err)), err);
}

void test_notify_validate_accepts_coinbase_only_block(void)
{
    stratum_v1_notify_view_t v = good_view();
    v.n_merkle_branches = 0;
    v.merkle_branches = NULL;
    char err[96] = {0};
    TEST_ASSERT_TRUE_MESSAGE(stratum_v1_notify_validate(&v, err, sizeof(err)), err);
}

void test_notify_validate_rejects_null_pointers(void)
{
    stratum_v1_notify_view_t v = good_view();
    char err[96] = {0};

    TEST_ASSERT_FALSE(stratum_v1_notify_validate(NULL, err, sizeof(err)));

    v.job_id = NULL;
    assert_rejected(v, "NULL job_id");
    v = good_view();
    v.prev_block_hash = NULL;
    assert_rejected(v, "NULL prev_block_hash");
    v = good_view();
    v.coinbase_1 = NULL;
    assert_rejected(v, "NULL coinbase_1");
    v = good_view();
    v.coinbase_2 = NULL;
    assert_rejected(v, "NULL coinbase_2");
    v = good_view();
    v.nbits = NULL;
    assert_rejected(v, "NULL nbits");
    v = good_view();
    v.version = NULL;
    assert_rejected(v, "NULL version");
    v = good_view();
    v.ntime = NULL;
    assert_rejected(v, "NULL ntime");
    v = good_view();
    v.n_merkle_branches = 5;
    v.merkle_branches = NULL;
    assert_rejected(v, "NULL merkle array with count > 0");
}

void test_notify_validate_rejects_bad_hex_lengths(void)
{
    stratum_v1_notify_view_t v = good_view();
    char err[96] = {0};

    v.prev_block_hash = "deadbeef";
    assert_rejected(v, "short prev_block_hash");

    v = good_view();
    v.prev_block_hash = "b9892e4ef50a39653f8571203a9bbc1caff64df83d7634bc000000010000000g";
    assert_rejected(v, "non-hex prev_block_hash");

    v = good_view();
    v.coinbase_2 = "abc";
    assert_rejected(v, "odd total coinbase length");

    v = good_view();
    {
        const char *short_branch[1] = {"013a6db4"};
        v.merkle_branches = short_branch;
        v.n_merkle_branches = 1;
        assert_rejected(v, "short merkle branch");
    }
}

void test_notify_validate_rejects_bad_job_id(void)
{
    stratum_v1_notify_view_t v = good_view();

    v.job_id = "";
    assert_rejected(v, "empty job_id");

    v.job_id = "0123456789012345678901234567890123"; /* 33 chars */
    assert_rejected(v, "over-long job_id");

    v.job_id = "bad\nid";
    assert_rejected(v, "control character in job_id");
}

void test_notify_validate_rejects_implausible_nbits(void)
{
    stratum_v1_notify_view_t v = good_view();

    v.nbits = "00000000";
    assert_rejected(v, "zero target");

    v = good_view();
    v.nbits = "020000c1";
    assert_rejected(v, "exponent below 0x03");

    v = good_view();
    v.nbits = "3f000001";
    assert_rejected(v, "exponent above 0x30");
}

void test_nbits_plausibility(void)
{
    TEST_ASSERT_TRUE(stratum_v1_nbits_is_plausible("1a1d4f9b"));
    TEST_ASSERT_TRUE(stratum_v1_nbits_is_plausible("1901ad9e"));
    TEST_ASSERT_FALSE(stratum_v1_nbits_is_plausible("00000000"));
    TEST_ASSERT_FALSE(stratum_v1_nbits_is_plausible(NULL));
}

void test_hex_helpers(void)
{
    TEST_ASSERT_TRUE(stratum_v1_hex_is_valid(kMerkle[0], 32));
    TEST_ASSERT_FALSE(stratum_v1_hex_is_valid(kMerkle[0], 31));
    TEST_ASSERT_FALSE(stratum_v1_hex_is_valid("", 32));
    TEST_ASSERT_FALSE(stratum_v1_hex_is_valid(NULL, 32));
    TEST_ASSERT_TRUE(stratum_v1_hex_num_is_valid("6ac179f3"));
    TEST_ASSERT_TRUE(stratum_v1_hex_num_is_valid("0"));
    TEST_ASSERT_FALSE(stratum_v1_hex_num_is_valid("6ac179f3a"));
    TEST_ASSERT_FALSE(stratum_v1_hex_num_is_valid(""));
    TEST_ASSERT_FALSE(stratum_v1_hex_num_is_valid("zzzz"));
}