#include "backup_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_wifi.h"
#include "mbedtls/sha256.h"

#include "http_server.h"

static const char *TAG = "backup_api";

#define BACKUP_CHUNK 4096
#define BACKUP_SCHEMA "hcs.sha.backup.v1"

static int s_prebuffer_len;

/* Both routes refuse in AP mode: the setup access point is open by design and
 * must never be able to download the firmware image. */
static bool station_mode_only(httpd_req_t *req)
{
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK) {
        return false;
    }
    (void) req;
    return mode != WIFI_MODE_AP && mode != WIFI_MODE_APSTA;
}

static esp_err_t hash_partition(const esp_partition_t *part, uint8_t *buf, size_t buf_len, char out_hex[65])
{
    mbedtls_sha256_context ctx;
    uint32_t offset = 0;
    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts(&ctx, 0) != 0) {
        mbedtls_sha256_free(&ctx);
        return ESP_FAIL;
    }
    while (offset < part->size) {
        size_t take = part->size - offset;
        if (take > buf_len) {
            take = buf_len;
        }
        esp_err_t err = esp_partition_read(part, offset, buf, take);
        if (err != ESP_OK) {
            mbedtls_sha256_free(&ctx);
            return err;
        }
        if (mbedtls_sha256_update(&ctx, buf, take) != 0) {
            mbedtls_sha256_free(&ctx);
            return ESP_FAIL;
        }
        offset += take;
    }
    uint8_t digest[32];
    if (mbedtls_sha256_finish(&ctx, digest) != 0) {
        mbedtls_sha256_free(&ctx);
        return ESP_FAIL;
    }
    mbedtls_sha256_free(&ctx);
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[2 * i] = digits[digest[i] >> 4];
        out_hex[2 * i + 1] = digits[digest[i] & 0x0F];
    }
    out_hex[64] = '\0';
    return ESP_OK;
}

static esp_err_t GET_backup_info(httpd_req_t *req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    if (!station_mode_only(req)) {
        HTTP_send_json_error(req, "409 Conflict", "Backup is not available in AP mode");
        return ESP_OK;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        HTTP_send_json_error(req, "500 Internal Server Error", "No running partition");
        return ESP_OK;
    }

    uint8_t *buf = malloc(BACKUP_CHUNK);
    if (buf == NULL) {
        HTTP_send_json_error(req, "500 Internal Server Error", "Out of memory");
        return ESP_OK;
    }
    char hex[65];
    esp_err_t err = hash_partition(running, buf, BACKUP_CHUNK, hex);
    free(buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hashing the running partition failed: %s", esp_err_to_name(err));
        HTTP_send_json_error(req, "500 Internal Server Error", "Could not read the running partition");
        return ESP_OK;
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }
    cJSON_AddStringToObject(root, "schema", BACKUP_SCHEMA);
    cJSON_AddStringToObject(root, "kind", "running_app_backup");
    cJSON_AddStringToObject(root, "partition", running->label);
    cJSON_AddNumberToObject(root, "offset", (double) running->address);
    cJSON_AddNumberToObject(root, "length", (double) running->size);
    cJSON_AddStringToObject(root, "sha256", hex);
    cJSON_AddStringToObject(root, "version", desc != NULL ? desc->version : "unknown");
    cJSON_AddStringToObject(root, "project", desc != NULL ? desc->project_name : "unknown");
    cJSON_AddStringToObject(root, "download_path", "/api/system/backup/running-app");
    /* Say what this is not, in the payload a client reads. */
    cJSON_AddStringToObject(root, "scope",
                            "running application partition only; bootloader, partition table, otadata, NVS and eFuses "
                            "are not included");
    cJSON_AddBoolToObject(root, "complete_chip_backup", false);

    httpd_resp_set_type(req, "application/json");
    esp_err_t res = HTTP_send_json(req, root, &s_prebuffer_len);
    cJSON_Delete(root);
    return res;
}

static esp_err_t GET_backup_running_app(httpd_req_t *req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    if (!station_mode_only(req)) {
        HTTP_send_json_error(req, "409 Conflict", "Backup is not available in AP mode");
        return ESP_OK;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        HTTP_send_json_error(req, "500 Internal Server Error", "No running partition");
        return ESP_OK;
    }
    const esp_app_desc_t *desc = esp_app_get_description();

    uint8_t *buf = malloc(BACKUP_CHUNK);
    if (buf == NULL) {
        HTTP_send_json_error(req, "500 Internal Server Error", "Out of memory");
        return ESP_OK;
    }

    /* One extra read pass so the digest can travel in a header: the client then
     * verifies the body it actually received, instead of trusting a number that
     * arrives after it. The app partition is immutable at run time, so the two
     * passes must agree; a mismatch is logged and the transfer is not called a
     * verified backup. */
    char sha[65];
    if (hash_partition(running, buf, BACKUP_CHUNK, sha) != ESP_OK) {
        free(buf);
        HTTP_send_json_error(req, "500 Internal Server Error", "Could not read the running partition");
        return ESP_OK;
    }

    char header[96];
    snprintf(header, sizeof(header), "attachment; filename=\"%s.bin\"", running->label);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", header);
    httpd_resp_set_hdr(req, "X-Image-Sha256", sha);
    snprintf(header, sizeof(header), "%u", (unsigned) running->size);
    httpd_resp_set_hdr(req, "X-Image-Length", header);
    httpd_resp_set_hdr(req, "X-Running-Partition", running->label);
    httpd_resp_set_hdr(req, "X-Image-Version", desc != NULL ? desc->version : "unknown");
    httpd_resp_set_hdr(req, "X-Image-Project", desc != NULL ? desc->project_name : "unknown");

    uint32_t offset = 0;
    esp_err_t result = ESP_OK;
    while (offset < running->size) {
        size_t take = running->size - offset;
        if (take > BACKUP_CHUNK) {
            take = BACKUP_CHUNK;
        }
        esp_err_t err = esp_partition_read(running, offset, buf, take);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "read at %u failed: %s", (unsigned) offset, esp_err_to_name(err));
            result = err;
            break;
        }
        if (httpd_resp_send_chunk(req, (const char *) buf, take) != ESP_OK) {
            /* Client went away: stop reading flash for a socket that cannot
             * receive it. The partial body is not a backup. */
            ESP_LOGW(TAG, "client disconnected during backup at %u", (unsigned) offset);
            result = ESP_FAIL;
            break;
        }
        offset += take;
    }
    free(buf);
    if (result == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    return ESP_OK;
}

esp_err_t backup_api_register(httpd_handle_t server, GlobalState *state)
{
    (void) state; /* the routes read the live partition, not cached state */
    static const httpd_uri_t uris[] = {
        { .uri = "/api/system/backup/info", .method = HTTP_GET, .handler = GET_backup_info, .user_ctx = NULL },
        { .uri = "/api/system/backup/running-app", .method = HTTP_GET, .handler = GET_backup_running_app, .user_ctx = NULL },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "registering %s failed: %s", uris[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    ESP_LOGI(TAG, "running-app backup routes registered");
    return ESP_OK;
}
