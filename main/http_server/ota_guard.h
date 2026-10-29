#ifndef OTA_GUARD_H_
#define OTA_GUARD_H_

#include <stddef.h>
#include <stdint.h>

/* Dependency-free decision logic for POST /api/system/OTA.
 *
 * The handler in http_server.c owns every ESP-IDF call; this module only decides
 * whether an upload is acceptable, so each rule can be exercised on the host
 * (simulation/test_ota_guard.c) without a device, a partition or a socket.
 *
 * Layout facts used here come from ESP-IDF v6.0.2 esp_app_format.h and
 * esp_app_desc.h: a 24-byte esp_image_header_t, one 8-byte segment header, then
 * esp_app_desc_t at offset 32 of every application image. A merged/factory image
 * starts with the second-stage bootloader instead, whose descriptor carries a
 * different magic, so it is rejected here rather than being written into an
 * application slot it cannot boot from. */

#define OTA_GUARD_IMAGE_MAGIC        0xE9
#define OTA_GUARD_APP_DESC_MAGIC     0xABCD5432u
#define OTA_GUARD_CHIP_ID_ESP32S3    0x0009

#define OTA_GUARD_CHIP_ID_OFFSET     12
#define OTA_GUARD_APP_DESC_OFFSET    32
#define OTA_GUARD_VERSION_OFFSET     (OTA_GUARD_APP_DESC_OFFSET + 16)
#define OTA_GUARD_PROJECT_OFFSET     (OTA_GUARD_APP_DESC_OFFSET + 48)
#define OTA_GUARD_FIELD_LEN          32

/* Bytes that must have arrived before ota_guard_check_header() can decide. */
#define OTA_GUARD_HEADER_LEN         (OTA_GUARD_PROJECT_OFFSET + OTA_GUARD_FIELD_LEN)

/* Consecutive receive timeouts tolerated before the upload is abandoned. With the
 * httpd default recv_wait_timeout of 5 s this is ~30 s of silence: long enough
 * for a slow Wi-Fi link, short enough that a vanished client cannot hold the
 * single HTTP server task (and with it the whole API) forever. */
#define OTA_GUARD_MAX_CONSECUTIVE_TIMEOUTS 6

typedef enum {
    OTA_GUARD_OK = 0,
    OTA_GUARD_NEED_MORE,          /* fewer than OTA_GUARD_HEADER_LEN bytes supplied */
    OTA_GUARD_BAD_ARG,
    OTA_GUARD_EMPTY,              /* Content-Length 0 or negative */
    OTA_GUARD_TOO_LARGE,          /* larger than the target slot */
    OTA_GUARD_BAD_IMAGE_MAGIC,    /* not an ESP image at all */
    OTA_GUARD_WRONG_CHIP,         /* built for a different ESP32 family */
    OTA_GUARD_NOT_APP_IMAGE,      /* ESP image, but no app descriptor: bootloader/merged/factory */
    OTA_GUARD_WRONG_PROJECT,      /* a valid app, but not this firmware */
} ota_guard_result_t;

/* Size gate, checked before anything is erased or written. */
ota_guard_result_t ota_guard_check_size(long content_len, size_t slot_size);

/* Header gate. `expected_project` is the running app's project_name, so the check
 * follows the build instead of a string baked in here. Pass NULL to skip it. */
ota_guard_result_t ota_guard_check_header(const uint8_t *buf, size_t len,
                                          uint16_t expected_chip_id,
                                          const char *expected_project);

/* Copies the image's version string (always NUL-terminated) for logging.
 * Requires OTA_GUARD_HEADER_LEN bytes; returns 0 on success. */
int ota_guard_image_version(const uint8_t *buf, size_t len, char *out, size_t out_len);

const char *ota_guard_result_str(ota_guard_result_t result);

#endif /* OTA_GUARD_H_ */
