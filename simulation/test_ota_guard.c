/*
 * Host tests for the OTA upload gate (main/http_server/ota_guard.c).
 *
 * The images below are assembled byte by byte from the ESP-IDF v6.0.2 struct
 * definitions (esp_image_header_t, esp_image_segment_header_t, esp_app_desc_t),
 * using literal offsets rather than the OTA_GUARD_* macros, so a wrong offset in
 * the module under test cannot also be wrong in its oracle.
 *
 * Optional real vectors: pass the freshly built OTA app image and, optionally, a
 * merged/factory image. The app image must be accepted and the factory image
 * rejected - this is the exact mix-up the gate exists to stop:
 *
 *   ./test_ota_guard ../build/esp-miner.bin ../esp-miner-merged.bin
 */

#include "ota_guard.h"

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

static void check_res(ota_guard_result_t got, ota_guard_result_t want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL: %s (got %s, want %s)\n", what, ota_guard_result_str(got), ota_guard_result_str(want));
    }
}

/* ---------------------------------------------------------------- fixtures */

#define IMG_LEN 512

/* A minimal ESP32-S3 application image head:
 *   [0]      magic 0xE9
 *   [1]      segment count
 *   [12..13] chip_id, little endian (ESP32-S3 = 9)
 *   [24..31] first segment header
 *   [32..35] esp_app_desc_t.magic_word 0xABCD5432, little endian
 *   [48..79] esp_app_desc_t.version
 *   [80..111] esp_app_desc_t.project_name */
static void make_app(uint8_t *img, const char *project, const char *version)
{
    memset(img, 0, IMG_LEN);
    img[0] = 0xE9;
    img[1] = 4;
    img[12] = 0x09;
    img[13] = 0x00;
    img[32] = 0x32;
    img[33] = 0x54;
    img[34] = 0xCD;
    img[35] = 0xAB;
    memcpy(img + 48, version, strlen(version));
    memcpy(img + 80, project, strlen(project));
}

/* The second-stage bootloader that sits at offset 0 of a merged/factory image:
 * same image magic and chip id, but esp_bootloader_desc_t (magic byte 0x50) in
 * place of the application descriptor. */
static void make_bootloader(uint8_t *img)
{
    memset(img, 0, IMG_LEN);
    img[0] = 0xE9;
    img[1] = 3;
    img[12] = 0x09;
    img[32] = 0x50;
}

/* ---------------------------------------------------------------- tests */

static void test_size_gate(void)
{
    printf("== size gate ==\n");
    const size_t slot = 4u * 1024u * 1024u;
    check_res(ota_guard_check_size(0, slot), OTA_GUARD_EMPTY, "zero Content-Length rejected");
    check_res(ota_guard_check_size(-1, slot), OTA_GUARD_EMPTY, "negative Content-Length rejected");
    check_res(ota_guard_check_size(111, slot), OTA_GUARD_NOT_APP_IMAGE, "body shorter than the header rejected");
    check_res(ota_guard_check_size(112, slot), OTA_GUARD_OK, "exactly one header accepted by the size gate");
    check_res(ota_guard_check_size((long) slot, slot), OTA_GUARD_OK, "image filling the slot accepted");
    check_res(ota_guard_check_size((long) slot + 1, slot), OTA_GUARD_TOO_LARGE, "one byte over the slot rejected");
    check_res(ota_guard_check_size(16L * 1024 * 1024, slot), OTA_GUARD_TOO_LARGE, "full 16 MiB flash dump rejected");
}

static void test_header_gate(void)
{
    printf("== header gate ==\n");
    uint8_t img[IMG_LEN];

    make_app(img, "esp-miner", "v4.0.2-stable");
    check_res(ota_guard_check_header(img, IMG_LEN, 9, "esp-miner"), OTA_GUARD_OK, "valid esp-miner app accepted");
    check_res(ota_guard_check_header(img, IMG_LEN, 9, NULL), OTA_GUARD_OK, "project check can be skipped");
    check_res(ota_guard_check_header(img, 112, 9, "esp-miner"), OTA_GUARD_OK, "exactly 112 bytes is enough");
    check_res(ota_guard_check_header(img, 111, 9, "esp-miner"), OTA_GUARD_NEED_MORE, "111 bytes is not enough");
    check_res(ota_guard_check_header(img, 0, 9, "esp-miner"), OTA_GUARD_NEED_MORE, "no bytes yet");
    check_res(ota_guard_check_header(NULL, IMG_LEN, 9, "esp-miner"), OTA_GUARD_BAD_ARG, "NULL buffer rejected");

    check_res(ota_guard_check_header(img, IMG_LEN, 5, "esp-miner"), OTA_GUARD_WRONG_CHIP, "ESP32-C3 expectation rejects S3 image");

    make_bootloader(img);
    check_res(ota_guard_check_header(img, IMG_LEN, 9, "esp-miner"), OTA_GUARD_NOT_APP_IMAGE, "bootloader/merged image rejected");

    make_app(img, "esp-miner", "v1");
    img[0] = 0x00;
    check_res(ota_guard_check_header(img, IMG_LEN, 9, "esp-miner"), OTA_GUARD_BAD_IMAGE_MAGIC, "missing 0xE9 rejected");

    make_app(img, "esp-miner", "v1");
    memset(img, 0xFF, IMG_LEN);
    check_res(ota_guard_check_header(img, IMG_LEN, 9, "esp-miner"), OTA_GUARD_BAD_IMAGE_MAGIC, "erased flash (0xFF) rejected");

    /* www.bin sent to the firmware endpoint: a SPIFFS image, starts with data. */
    memset(img, 0, IMG_LEN);
    memcpy(img, "\x00\x00\x00\x00spiffs", 10);
    check_res(ota_guard_check_header(img, IMG_LEN, 9, "esp-miner"), OTA_GUARD_BAD_IMAGE_MAGIC, "www.bin on the firmware endpoint rejected");

    make_app(img, "hello_world", "v1");
    check_res(ota_guard_check_header(img, IMG_LEN, 9, "esp-miner"), OTA_GUARD_WRONG_PROJECT, "foreign application rejected");

    make_app(img, "esp-miner-x", "v1");
    check_res(ota_guard_check_header(img, IMG_LEN, 9, "esp-miner"), OTA_GUARD_WRONG_PROJECT, "project with matching prefix rejected");

    make_app(img, "esp", "v1");
    check_res(ota_guard_check_header(img, IMG_LEN, 9, "esp-miner"), OTA_GUARD_WRONG_PROJECT, "truncated project name rejected");

    make_app(img, "", "v1");
    check_res(ota_guard_check_header(img, IMG_LEN, 9, "esp-miner"), OTA_GUARD_WRONG_PROJECT, "empty project name rejected");

    make_app(img, "esp-miner", "v1");
    memset(img + 80, 'A', 32);
    check_res(ota_guard_check_header(img, IMG_LEN, 9, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"), OTA_GUARD_WRONG_PROJECT,
              "unterminated 32-byte project field rejected");

    /* App descriptor magic off by one byte. */
    make_app(img, "esp-miner", "v1");
    img[35] = 0xAC;
    check_res(ota_guard_check_header(img, IMG_LEN, 9, "esp-miner"), OTA_GUARD_NOT_APP_IMAGE, "corrupted descriptor magic rejected");
}

static void test_version_extract(void)
{
    printf("== version extraction ==\n");
    uint8_t img[IMG_LEN];
    char out[40];

    make_app(img, "esp-miner", "v4.0.2-stable");
    check(ota_guard_image_version(img, IMG_LEN, out, sizeof(out)) == 0 && strcmp(out, "v4.0.2-stable") == 0, "version read");

    char small[5];
    check(ota_guard_image_version(img, IMG_LEN, small, sizeof(small)) == 0 && strcmp(small, "v4.0") == 0, "version truncated, terminated");

    memset(img + 48, 'X', 32);
    check(ota_guard_image_version(img, IMG_LEN, out, sizeof(out)) == 0 && strlen(out) == 32, "unterminated version bounded at 32");

    check(ota_guard_image_version(img, 100, out, sizeof(out)) != 0, "short buffer refused");
    check(ota_guard_image_version(img, IMG_LEN, out, 0) != 0, "zero-length output refused");
}

/* Randomised headers must never crash and must only ever be accepted when every
 * documented field matches. */
static void test_random_headers(void)
{
    printf("== randomised headers ==\n");
    uint8_t img[IMG_LEN];
    unsigned int seed = 12345;
    int accepted = 0;
    for (int i = 0; i < 200000; i++) {
        for (int j = 0; j < 128; j++) {
            seed = seed * 1103515245u + 12345u;
            img[j] = (uint8_t) (seed >> 16);
        }
        /* Bias a share of the samples towards plausible heads. */
        if (i % 4 == 0) img[0] = 0xE9;
        if (i % 8 == 0) { img[12] = 9; img[13] = 0; }
        if (i % 16 == 0) { img[32] = 0x32; img[33] = 0x54; img[34] = 0xCD; img[35] = 0xAB; }
        size_t len = (size_t) (seed % 129);
        ota_guard_result_t r = ota_guard_check_header(img, len, 9, "esp-miner");
        if (r == OTA_GUARD_OK) {
            accepted++;
            int ok = len >= 112 && img[0] == 0xE9 && img[12] == 9 && img[13] == 0 &&
                     img[32] == 0x32 && img[33] == 0x54 && img[34] == 0xCD && img[35] == 0xAB &&
                     memcmp(img + 80, "esp-miner", 10) == 0;
            check(ok, "random header accepted only when every field matches");
        }
    }
    check(accepted == 0, "no random header carries a full esp-miner descriptor");
}

/* ---------------------------------------------------------------- real vectors */

static int load_head(const char *path, uint8_t *buf, size_t want, size_t *got, long *total)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    *got = fread(buf, 1, want, f);
    fseek(f, 0, SEEK_END);
    *total = ftell(f);
    fclose(f);
    return 0;
}

static void test_real_file(const char *path, int expect_ok)
{
    uint8_t buf[IMG_LEN];
    size_t got = 0;
    long total = 0;
    printf("== real image %s (expect %s) ==\n", path, expect_ok ? "accept" : "reject");
    if (load_head(path, buf, sizeof(buf), &got, &total) != 0) {
        check(0, "image file readable");
        return;
    }
    ota_guard_result_t size_res = ota_guard_check_size(total, 4u * 1024u * 1024u);
    ota_guard_result_t hdr_res = ota_guard_check_header(buf, got, 9, "esp-miner");
    char version[33] = "";
    ota_guard_image_version(buf, got, version, sizeof(version));
    printf("    size=%ld size_gate=%s header_gate=%s version=\"%s\"\n", total, ota_guard_result_str(size_res),
           ota_guard_result_str(hdr_res), version);
    if (expect_ok) {
        check_res(size_res, OTA_GUARD_OK, "real app image passes size gate");
        check_res(hdr_res, OTA_GUARD_OK, "real app image passes header gate");
    } else {
        check(size_res != OTA_GUARD_OK || hdr_res != OTA_GUARD_OK, "real factory/merged image is refused");
    }
}

int main(int argc, char **argv)
{
    printf("ota_guard host tests\n");
    test_size_gate();
    test_header_gate();
    test_version_extract();
    test_random_headers();
    if (argc > 1) test_real_file(argv[1], 1);
    if (argc > 2) test_real_file(argv[2], 0);

    printf("%d checks, %d failures\n", checks, failures);
    if (failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    return 1;
}
