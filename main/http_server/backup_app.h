#ifndef BACKUP_APP_H_
#define BACKUP_APP_H_

#include <esp_err.h>
#include <esp_http_server.h>

typedef struct GlobalState GlobalState;

/* Read-only WLAN backup of the *running application partition*.
 *
 *   GET /api/system/backup/info          -> JSON metadata (offset, size, sha256, version, project)
 *   GET /api/system/backup/running-app   -> the raw bytes of the running partition
 *
 * Why this exists: the OTA endpoint accepts an application image, but nothing in
 * the stock firmware lets an operator take a copy of what is actually running
 * before flashing something else. A full-chip USB dump remains the primary
 * rescue path; this is the WLAN-accessible, hash-verified copy.
 *
 * Scope, stated honestly:
 *   - it serves the whole running partition as-is, including the unused 0xFF
 *     tail, so the bytes can be written back to the same offset unchanged;
 *   - it never reads a partition other than the running one (no offsets, no
 *     lengths from the client), so it cannot be used to exfiltrate NVS, the web
 *     asset partition or another OTA slot;
 *   - it does not expose Wi-Fi, pool or wallet configuration.
 *
 * This is not a complete chip backup: bootloader, partition table, otadata,
 * NVS and eFuses are outside it. The report must never call it one.
 */
esp_err_t backup_api_register(httpd_handle_t server, GlobalState *state);

#endif /* BACKUP_APP_H_ */
