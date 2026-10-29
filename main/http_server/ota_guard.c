#include "ota_guard.h"

#include <string.h>

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t) (p[0] | (p[1] << 8));
}

/* strnlen is POSIX, not C11; the host tests build with -std=c11. */
static size_t field_len(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

ota_guard_result_t ota_guard_check_size(long content_len, size_t slot_size)
{
    if (content_len <= 0) {
        return OTA_GUARD_EMPTY;
    }
    /* An image must at least carry the header we are about to inspect. */
    if ((size_t) content_len < OTA_GUARD_HEADER_LEN) {
        return OTA_GUARD_NOT_APP_IMAGE;
    }
    if ((unsigned long) content_len > slot_size) {
        return OTA_GUARD_TOO_LARGE;
    }
    return OTA_GUARD_OK;
}

ota_guard_result_t ota_guard_check_header(const uint8_t *buf, size_t len,
                                          uint16_t expected_chip_id,
                                          const char *expected_project)
{
    if (buf == NULL) {
        return OTA_GUARD_BAD_ARG;
    }
    if (len < OTA_GUARD_HEADER_LEN) {
        return OTA_GUARD_NEED_MORE;
    }
    if (buf[0] != OTA_GUARD_IMAGE_MAGIC) {
        return OTA_GUARD_BAD_IMAGE_MAGIC;
    }
    if (read_le16(buf + OTA_GUARD_CHIP_ID_OFFSET) != expected_chip_id) {
        return OTA_GUARD_WRONG_CHIP;
    }
    if (read_le32(buf + OTA_GUARD_APP_DESC_OFFSET) != OTA_GUARD_APP_DESC_MAGIC) {
        return OTA_GUARD_NOT_APP_IMAGE;
    }
    if (expected_project != NULL) {
        const char *project = (const char *) (buf + OTA_GUARD_PROJECT_OFFSET);
        size_t n = field_len(project, OTA_GUARD_FIELD_LEN);
        /* A full 32-byte field without terminator is malformed, not a match. */
        if (n == OTA_GUARD_FIELD_LEN || n != strlen(expected_project) || memcmp(project, expected_project, n) != 0) {
            return OTA_GUARD_WRONG_PROJECT;
        }
    }
    return OTA_GUARD_OK;
}

int ota_guard_image_version(const uint8_t *buf, size_t len, char *out, size_t out_len)
{
    if (buf == NULL || out == NULL || out_len == 0 || len < OTA_GUARD_HEADER_LEN) {
        return -1;
    }
    size_t n = field_len((const char *) (buf + OTA_GUARD_VERSION_OFFSET), OTA_GUARD_FIELD_LEN);
    if (n >= out_len) {
        n = out_len - 1;
    }
    memcpy(out, buf + OTA_GUARD_VERSION_OFFSET, n);
    out[n] = '\0';
    return 0;
}

const char *ota_guard_result_str(ota_guard_result_t result)
{
    switch (result) {
        case OTA_GUARD_OK:              return "ok";
        case OTA_GUARD_NEED_MORE:       return "incomplete header";
        case OTA_GUARD_BAD_ARG:         return "invalid argument";
        case OTA_GUARD_EMPTY:           return "empty upload";
        case OTA_GUARD_TOO_LARGE:       return "image larger than OTA slot";
        case OTA_GUARD_BAD_IMAGE_MAGIC: return "not an ESP firmware image";
        case OTA_GUARD_WRONG_CHIP:      return "image built for a different chip";
        case OTA_GUARD_NOT_APP_IMAGE:   return "not an OTA app image (factory/merged?)";
        case OTA_GUARD_WRONG_PROJECT:   return "image is not esp-miner firmware";
    }
    return "unknown";
}
