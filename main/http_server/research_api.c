#include "research_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "global_state.h"
#include "http_server.h"
#include "research_guard.h"
#include "research_sha.h"

#ifdef CONFIG_TIMSMINER_SHA_RESEARCH

static const char *TAG = "research_api";

#define RESEARCH_STATUS_SCHEMA "hcs.sha.miner.v1"
#define RESEARCH_RESULT_SCHEMA "hcs.sha.device_sha256.v1"

/* Scratch for cJSON's print buffer. One HTTP task, so one buffer. It must start
 * positive: cJSON_PrintBuffered() allocates the prebuffer up front, and
 * malloc(0) returns NULL here, which would turn every JSON response into a
 * failed handler without a body. 256 matches the other API handlers. */
static int s_prebuffer_len = 256;

/* The HTTP task has an 8 KB stack, so the trace round buffer lives in .bss. 64
 * rounds is the whole compression function; a smaller `rounds` request simply
 * uses the front of it. */
static research_sha_round_t s_rounds[RESEARCH_SHA_ROUNDS];
/* One request body at a time: the httpd handler is a single task, but the flag
 * makes a second overlapping request a 409 instead of a corrupted buffer. */
static char s_body[2048];
static bool s_body_in_use;
static research_sha_trace_t s_trace;

static research_guard_t s_guard;
static GlobalState *s_state;
static bool s_ready;

static uint64_t now_ms(void)
{
    return (uint64_t) (esp_timer_get_time() / 1000);
}

/* --- environment sampling ------------------------------------------------ */

/* Everything the guard needs, sampled at the moment of the decision. Anything
 * that cannot be read is reported as unknown, never as a plausible default. */
static research_guard_conditions_t sample_conditions(void)
{
    research_guard_conditions_t c;
    memset(&c, 0, sizeof(c));
    if (s_state == NULL) {
        return c;
    }

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const esp_app_desc_t *desc = esp_app_get_description();
    /* Identity is not taken on trust from the client: this build must be the
     * esp-miner application on an ESP32-S3 with a known board and an ASIC that
     * already initialised in this boot. */
    c.identity_verified = desc != NULL && chip.model == CHIP_ESP32S3 && strcmp(desc->project_name, "esp-miner") == 0 &&
                          s_state->ASIC_initalized && s_state->DEVICE_CONFIG.family.asic.name != NULL;

    c.pool_connected = s_state->SYSTEM_MODULE.is_connected || s_state->SYSTEM_MODULE.is_using_fallback;
    c.share_in_flight = s_state->SYSTEM_MODULE.shares_pending > 0;

    /* Read the values the power-monitoring task already publishes instead of
     * touching I2C/PMBus from the HTTP task: the sensor bus is owned by that
     * task, and a research request must never be able to stall it. A value the
     * monitoring task has not filled in yet is <= 0, which is "unknown" and
     * therefore unsafe. */
    float temp = s_state->POWER_MANAGEMENT_MODULE.chip_temp_avg;
    c.thermal_known = temp > 0.0f && temp < 200.0f;
    c.temperature_c = c.thermal_known ? temp : 0.0f;

    float power_w = s_state->POWER_MANAGEMENT_MODULE.power;
    c.power_known = power_w > 0.0f && power_w < 1000.0f;
    c.power_w = c.power_known ? power_w : 0.0f;

    return c;
}

static research_guard_config_t configured_guard(void)
{
    research_guard_config_t cfg = research_guard_config_default();
    cfg.max_jobs = CONFIG_TIMSMINER_SHA_RESEARCH_MAX_JOBS;
    cfg.max_runtime_ms = (uint64_t) CONFIG_TIMSMINER_SHA_RESEARCH_MAX_RUNTIME_S * 1000ull;
    cfg.max_temperature_c = (float) CONFIG_TIMSMINER_SHA_RESEARCH_TEMPERATURE_C;
    return cfg;
}

/* --- helpers ------------------------------------------------------------- */

static void add_nullable_number(cJSON *root, const char *name, bool known, double value)
{
    if (known) {
        cJSON_AddNumberToObject(root, name, value);
    } else {
        cJSON_AddNullToObject(root, name);
    }
}

static void add_device_json(cJSON *root)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    cJSON_AddStringToObject(root, "device", "local-allowlist"); /* no MAC/IP in the payload */
    cJSON_AddStringToObject(root, "mcu", "ESP32-S3");
    if (s_state != NULL && s_state->DEVICE_CONFIG.family.asic.name != NULL) {
        cJSON_AddStringToObject(root, "asic_model", s_state->DEVICE_CONFIG.family.asic.name);
    } else {
        cJSON_AddNullToObject(root, "asic_model");
    }
    if (s_state != NULL && s_state->DEVICE_CONFIG.board_version != NULL) {
        cJSON_AddStringToObject(root, "board_version", s_state->DEVICE_CONFIG.board_version);
    } else {
        cJSON_AddNullToObject(root, "board_version");
    }
    cJSON_AddStringToObject(root, "firmware_version", desc != NULL ? desc->version : "unknown");
    if (desc != NULL) {
        char build[17];
        research_sha_bytes_to_hex((const uint8_t *) desc->app_elf_sha256, 8, build, sizeof(build));
        cJSON_AddStringToObject(root, "firmware_build_id", build);
    } else {
        cJSON_AddNullToObject(root, "firmware_build_id");
    }
    cJSON_AddStringToObject(root, "running_partition", running != NULL ? running->label : "unknown");
}

/* Reads a JSON body into the shared buffer. Returns NULL on any problem and has
 * already sent the error response. */
static cJSON *read_json_body(httpd_req_t *req, const char *bad_status)
{
    if (s_body_in_use) {
        HTTP_send_json_error(req, "409 Conflict", "Another research request is in progress");
        return NULL;
    }
    if (req->content_len <= 0 || req->content_len >= (int) sizeof(s_body)) {
        httpd_resp_send_err(req, req->content_len <= 0 ? HTTPD_400_BAD_REQUEST : HTTPD_413_CONTENT_TOO_LARGE,
                            "Research request body missing or too large");
        return NULL;
    }

    s_body_in_use = true;
    int remaining = req->content_len;
    int received = 0;
    int timeouts = 0;
    while (remaining > 0) {
        int got = httpd_req_recv(req, s_body + received, remaining);
        if (got == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
            continue;
        }
        if (got <= 0) {
            s_body_in_use = false;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Body read failed");
            return NULL;
        }
        timeouts = 0;
        received += got;
        remaining -= got;
    }
    s_body[received] = '\0';

    cJSON *root = cJSON_Parse(s_body);
    s_body_in_use = false;
    if (root == NULL) {
        HTTP_send_json_error(req, bad_status, "Invalid JSON body");
        return NULL;
    }
    return root;
}

/* Sends the command result as JSON and returns ESP_OK for the httpd task. */
static esp_err_t send_command_result(httpd_req_t *req, research_guard_result_t result, const char *what)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }
    cJSON_AddStringToObject(root, "schema", RESEARCH_STATUS_SCHEMA);
    cJSON_AddStringToObject(root, "command", what);
    cJSON_AddStringToObject(root, "result", research_guard_result_str(result));
    cJSON_AddStringToObject(root, "state", research_guard_state_str(s_guard.state));
    cJSON_AddBoolToObject(root, "accepted", result == RESEARCH_GUARD_OK);
    if (result != RESEARCH_GUARD_OK) {
        httpd_resp_set_status(req, "409 Conflict");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t res = HTTP_send_json(req, root, &s_prebuffer_len);
    cJSON_Delete(root);
    return res;
}

/* --- GET /api/research/status -------------------------------------------- */

static esp_err_t GET_research_status(httpd_req_t *req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    if (!s_ready) {
        HTTP_send_json_error(req, "503 Service Unavailable", "System state not initialised");
        return ESP_OK;
    }

    research_guard_conditions_t cond = sample_conditions();
    /* Every status poll is also a watchdog tick: if the device became unsafe
     * while a session was armed, the session is already disarmed by the time the
     * client reads this document. */
    research_guard_evaluate(&s_guard, now_ms(), &cond);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }
    cJSON_AddStringToObject(root, "schema", RESEARCH_STATUS_SCHEMA);
    cJSON_AddStringToObject(root, "kind", "research_status");
    add_device_json(root);

    cJSON_AddBoolToObject(root, "research_enabled", true);
    cJSON_AddBoolToObject(root, "research_armed",
                          s_guard.state == RESEARCH_GUARD_ARMED || s_guard.state == RESEARCH_GUARD_RUNNING ||
                              s_guard.state == RESEARCH_GUARD_PAUSED);
    cJSON_AddStringToObject(root, "research_state", research_guard_state_str(s_guard.state));
    cJSON_AddStringToObject(root, "last_refusal", research_guard_result_str(s_guard.last_refusal));

    cJSON_AddBoolToObject(root, "pool_connected", cond.pool_connected);
    cJSON_AddBoolToObject(root, "share_in_flight", cond.share_in_flight);
    cJSON_AddBoolToObject(root, "identity_verified", cond.identity_verified);
    cJSON_AddBoolToObject(root, "power_verified", cond.power_known);

    cJSON_AddNumberToObject(root, "active_jobs", s_guard.state == RESEARCH_GUARD_RUNNING ? 1 : 0);
    cJSON_AddNumberToObject(root, "jobs_started", s_guard.jobs_started);
    cJSON_AddNumberToObject(root, "jobs_completed", s_guard.jobs_completed);
    cJSON_AddNumberToObject(root, "remaining_jobs", research_guard_remaining_jobs(&s_guard));
    cJSON_AddNumberToObject(root, "job_budget", s_guard.cfg.max_jobs);
    cJSON_AddNumberToObject(root, "time_budget_ms", (double) s_guard.cfg.max_runtime_ms);
    cJSON_AddNumberToObject(root, "session_active_ms", (double) research_guard_active_ms(&s_guard, now_ms()));

    add_nullable_number(root, "temperature_c", cond.thermal_known, cond.temperature_c);
    add_nullable_number(root, "temperature_ceiling_c", true, s_guard.cfg.max_temperature_c);
    add_nullable_number(root, "power_w", cond.power_known, cond.power_w);
    add_nullable_number(root, "frequency_mhz", s_state->POWER_MANAGEMENT_MODULE.actual_frequency > 0.0f,
                        s_state->POWER_MANAGEMENT_MODULE.actual_frequency);
    add_nullable_number(root, "voltage_mv", s_state->POWER_MANAGEMENT_MODULE.voltage > 0.0f,
                        s_state->POWER_MANAGEMENT_MODULE.voltage);
    add_nullable_number(root, "hashrate_ghs", s_state->SYSTEM_MODULE.current_hashrate > 0.0f,
                        s_state->SYSTEM_MODULE.current_hashrate);

    /* The clock exists only after SNTP sync; a fabricated timestamp would be
     * worse than null. */
    add_nullable_number(root, "timestamp_unix", s_state->SYSTEM_MODULE.lastClockSync > 0,
                        (double) s_state->SYSTEM_MODULE.lastClockSync);
    cJSON_AddNumberToObject(root, "uptime_s", (double) (esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(root, "pairing_channel", "serial_log");

    httpd_resp_set_type(req, "application/json");
    esp_err_t res = HTTP_send_json(req, root, &s_prebuffer_len);
    cJSON_Delete(root);
    return res;
}

/* --- GET /api/research/capabilities -------------------------------------- */

static esp_err_t GET_research_capabilities(httpd_req_t *req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }
    cJSON_AddStringToObject(root, "schema", RESEARCH_STATUS_SCHEMA);
    cJSON_AddStringToObject(root, "kind", "research_capabilities");
    add_device_json(root);

    cJSON *features = cJSON_CreateArray();
    cJSON_AddItemToArray(features, cJSON_CreateString("sha256"));
    cJSON_AddItemToArray(features, cJSON_CreateString("sha256d"));
    cJSON_AddItemToArray(features, cJSON_CreateString("round_trace"));
    cJSON_AddItemToArray(features, cJSON_CreateString("reduced_rounds"));
    cJSON_AddItemToObject(root, "features", features);

    cJSON_AddNumberToObject(root, "max_trace_rounds", RESEARCH_SHA_ROUNDS);
    cJSON_AddNumberToObject(root, "max_message_bytes", 512);
    cJSON_AddStringToObject(root, "trace_schema", "hcs.sha.trace.v1");
    cJSON_AddStringToObject(root, "trace_source", "SOFTWARE_REPLAY");
    cJSON_AddStringToObject(root, "compute", "software_cpu");

    /* The honest boundary, in the payload a client reads before it asks for
     * anything. */
    cJSON *limits = cJSON_CreateObject();
    cJSON_AddStringToObject(limits, "asic_access", "none");
    cJSON_AddStringToObject(limits, "asic_note",
                            "The BM1366 executes SHA-256d internally and returns nonces. No round state, intermediate "
                            "state or arbitrary message block can be read from or written to the ASIC.");
    cJSON_AddStringToObject(limits, "workload", "software SHA-256 of a caller-supplied 64-byte block or message");
    cJSON_AddItemToObject(root, "limitations", limits);

    cJSON *endpoints = cJSON_CreateArray();
    const char *uris[] = {"/api/research/status", "/api/research/capabilities", "/api/research/challenge",
                          "/api/research/arm", "/api/research/pause", "/api/research/resume",
                          "/api/research/cancel", "/api/research/disarm", "/api/research/sha256"};
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        cJSON_AddItemToArray(endpoints, cJSON_CreateString(uris[i]));
    }
    cJSON_AddItemToObject(root, "endpoints", endpoints);

    httpd_resp_set_type(req, "application/json");
    esp_err_t res = HTTP_send_json(req, root, &s_prebuffer_len);
    cJSON_Delete(root);
    return res;
}

/* --- POST /api/research/challenge ---------------------------------------- */

static esp_err_t POST_research_challenge(httpd_req_t *req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    if (!s_ready) {
        HTTP_send_json_error(req, "503 Service Unavailable", "System state not initialised");
        return ESP_OK;
    }

    /* Six digits from the hardware RNG. The token is deliberately NOT sent in the
     * response: that would make the challenge pointless. It goes to the serial
     * log (and therefore the operator's console), and the operator types it back
     * into /arm. A device whose log endpoint is reachable by the same client that
     * arms it gets a weaker pairing than one read over USB serial; that
     * limitation is documented rather than papered over. */
    uint32_t token = esp_random() % 1000000u;
    if (token == 0) {
        token = 1;
    }
    research_guard_offer_token(&s_guard, token, now_ms());
    ESP_LOGW(TAG, "research pairing token: %06u (valid for %u s, read it on the serial console)",
             (unsigned) token, (unsigned) (s_guard.cfg.token_ttl_ms / 1000u));

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }
    cJSON_AddStringToObject(root, "schema", RESEARCH_STATUS_SCHEMA);
    cJSON_AddStringToObject(root, "result", "challenge_issued");
    cJSON_AddNumberToObject(root, "digits", 6);
    cJSON_AddNumberToObject(root, "expires_in_s", (double) (s_guard.cfg.token_ttl_ms / 1000u));
    cJSON_AddStringToObject(root, "channel", "serial_log");
    cJSON_AddStringToObject(root, "state", research_guard_state_str(s_guard.state));
    httpd_resp_set_type(req, "application/json");
    esp_err_t res = HTTP_send_json(req, root, &s_prebuffer_len);
    cJSON_Delete(root);
    return res;
}

/* --- POST /api/research/arm ---------------------------------------------- */

static esp_err_t POST_research_arm(httpd_req_t *req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    if (!s_ready) {
        HTTP_send_json_error(req, "503 Service Unavailable", "System state not initialised");
        return ESP_OK;
    }

    cJSON *root = read_json_body(req, "409 Conflict");
    if (root == NULL) {
        return ESP_OK;
    }
    cJSON *token_item = cJSON_GetObjectItemCaseSensitive(root, "token");
    if (!cJSON_IsString(token_item) || token_item->valuestring == NULL) {
        cJSON_Delete(root);
        HTTP_send_json_error(req, "400 Bad Request", "token is required");
        return ESP_OK;
    }
    char *end = NULL;
    unsigned long token = strtoul(token_item->valuestring, &end, 10);
    if (end == token_item->valuestring || *end != '\0' || token > 0xFFFFFFFFul) {
        cJSON_Delete(root);
        HTTP_send_json_error(req, "400 Bad Request", "token must be a decimal integer");
        return ESP_OK;
    }
    cJSON_Delete(root);

    research_guard_conditions_t cond = sample_conditions();
    research_guard_result_t result = research_guard_arm(&s_guard, (uint32_t) token, now_ms(), &cond);
    /* A failed arm deliberately does NOT burn the offered token: otherwise any
     * client on the LAN could invalidate the operator's pending challenge with a
     * single wrong guess. Arming still consumes the token on success. */
    return send_command_result(req, result, "arm");
}

/* --- POST /api/research/{pause,resume,cancel,disarm} --------------------- */

static esp_err_t POST_research_pause(httpd_req_t *req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    return send_command_result(req, research_guard_pause(&s_guard, now_ms()), "pause");
}

static esp_err_t POST_research_resume(httpd_req_t *req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    research_guard_conditions_t cond = sample_conditions();
    return send_command_result(req, research_guard_resume(&s_guard, now_ms(), &cond), "resume");
}

static esp_err_t POST_research_cancel(httpd_req_t *req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    return send_command_result(req, research_guard_cancel(&s_guard, now_ms()), "cancel");
}

static esp_err_t POST_research_disarm(httpd_req_t *req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    return send_command_result(req, research_guard_disarm(&s_guard, now_ms()), "disarm");
}

/* --- POST /api/research/sha256 ------------------------------------------- */

typedef struct {
    httpd_req_t *req;
} chunk_writer_t;

static int chunk_write(void *ctx, const char *data, size_t len)
{
    chunk_writer_t *w = (chunk_writer_t *) ctx;
    /* A client that vanished must stop the computation, not be written into. */
    return httpd_resp_send_chunk(w->req, data, len) == ESP_OK ? 0 : -1;
}

static esp_err_t trace_block_response(httpd_req_t *req, const uint8_t block[64], unsigned rounds, bool feed_forward)
{
    research_sha_state_t state;
    research_sha_state_init(&state);
    s_trace.rounds = s_rounds;
    s_trace.rounds_capacity = RESEARCH_SHA_ROUNDS;
    if (research_sha_trace_run(&s_trace, &state, block, rounds, feed_forward) != 0) {
        HTTP_send_json_error(req, "500 Internal Server Error", "Trace failed");
        return ESP_OK;
    }

    chunk_writer_t writer = { .req = req };
    char head[512];
    const esp_app_desc_t *desc = esp_app_get_description();
    int n = snprintf(head, sizeof(head),
                     "{\"schema\":\"%s\",\"kind\":\"block_trace\",\"compute\":\"software_cpu\",\"asic_access\":\"none\","
                     "\"firmware_version\":\"%s\",\"trace\":",
                     RESEARCH_RESULT_SCHEMA, desc != NULL ? desc->version : "unknown");
    if (n <= 0 || (size_t) n >= sizeof(head)) {
        HTTP_send_json_error(req, "500 Internal Server Error", "Envelope too large");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    if (httpd_resp_send_chunk(req, head, n) != ESP_OK) {
        return ESP_OK;
    }
    if (research_sha_trace_write_json(&s_trace, chunk_write, &writer) != 0) {
        /* The trace is already half sent; close the response rather than pretend
         * the document is complete. */
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }
    httpd_resp_send_chunk(req, "}", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t POST_research_sha256(httpd_req_t *req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    if (!s_ready) {
        HTTP_send_json_error(req, "503 Service Unavailable", "System state not initialised");
        return ESP_OK;
    }

    /* The oracle is useful whether or not a session is armed, but only when the
     * device is otherwise safe: an unarmed device with a pool running must not be
     * asked to spend CPU on research while it is hashing. */
    research_guard_conditions_t cond = sample_conditions();
    if (!cond.identity_verified) {
        HTTP_send_json_error(req, "409 Conflict", "identity_unverified");
        return ESP_OK;
    }
    if (cond.pool_connected || cond.share_in_flight) {
        HTTP_send_json_error(req, "409 Conflict", "pool_active");
        return ESP_OK;
    }
    if (!cond.thermal_known || cond.temperature_c > s_guard.cfg.max_temperature_c) {
        HTTP_send_json_error(req, "409 Conflict", "thermal_unsafe");
        return ESP_OK;
    }

    cJSON *root = read_json_body(req, "400 Bad Request");
    if (root == NULL) {
        return ESP_OK;
    }

    cJSON *rounds_item = cJSON_GetObjectItemCaseSensitive(root, "rounds");
    unsigned rounds = RESEARCH_SHA_ROUNDS;
    if (cJSON_IsNumber(rounds_item)) {
        if (rounds_item->valuedouble < 0 || rounds_item->valuedouble > RESEARCH_SHA_ROUNDS) {
            cJSON_Delete(root);
            HTTP_send_json_error(req, "400 Bad Request", "rounds must be 0..64");
            return ESP_OK;
        }
        rounds = (unsigned) rounds_item->valuedouble;
    }
    cJSON *ff_item = cJSON_GetObjectItemCaseSensitive(root, "feed_forward");
    bool feed_forward = true;
    if (cJSON_IsBool(ff_item)) {
        feed_forward = cJSON_IsTrue(ff_item);
    }

    cJSON *block_item = cJSON_GetObjectItemCaseSensitive(root, "block_hex");
    cJSON *message_item = cJSON_GetObjectItemCaseSensitive(root, "message_hex");
    cJSON *dbl_item = cJSON_GetObjectItemCaseSensitive(root, "sha256d");
    bool sha256d = cJSON_IsTrue(dbl_item);

    if (cJSON_IsString(block_item) && block_item->valuestring != NULL) {
        uint8_t block[RESEARCH_SHA_BLOCK_LEN];
        if (research_sha_hex_to_bytes(block_item->valuestring, block, sizeof(block)) != RESEARCH_SHA_BLOCK_LEN) {
            cJSON_Delete(root);
            HTTP_send_json_error(req, "400 Bad Request", "block_hex must be exactly 64 bytes of hex");
            return ESP_OK;
        }
        cJSON_Delete(root);
        return trace_block_response(req, block, rounds, feed_forward);
    }

    if (cJSON_IsString(message_item) && message_item->valuestring != NULL) {
        static uint8_t message[512];
        int len = research_sha_hex_to_bytes(message_item->valuestring, message, sizeof(message));
        if (len < 0) {
            cJSON_Delete(root);
            HTTP_send_json_error(req, "400 Bad Request", "message_hex must be even-length hex of at most 512 bytes");
            return ESP_OK;
        }
        uint8_t digest[RESEARCH_SHA_DIGEST_LEN];
        if (sha256d) {
            research_sha256d(message, (size_t) len, digest);
        } else {
            research_sha256(message, (size_t) len, digest);
        }
        char hex[RESEARCH_SHA_DIGEST_LEN * 2 + 1];
        research_sha_bytes_to_hex(digest, sizeof(digest), hex, sizeof(hex));
        cJSON_Delete(root);

        cJSON *out = cJSON_CreateObject();
        if (out == NULL) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
            return ESP_OK;
        }
        cJSON_AddStringToObject(out, "schema", RESEARCH_RESULT_SCHEMA);
        cJSON_AddStringToObject(out, "kind", "message_digest");
        cJSON_AddStringToObject(out, "compute", "software_cpu");
        add_device_json(out);
        cJSON_AddStringToObject(out, "algorithm", sha256d ? "sha256d" : "sha256");
        cJSON_AddNumberToObject(out, "message_bytes", len);
        cJSON_AddStringToObject(out, "digest", hex);
        httpd_resp_set_type(req, "application/json");
        esp_err_t res = HTTP_send_json(req, out, &s_prebuffer_len);
        cJSON_Delete(out);
        return res;
    }

    cJSON_Delete(root);
    HTTP_send_json_error(req, "400 Bad Request", "provide block_hex or message_hex");
    return ESP_OK;
}

/* --- registration -------------------------------------------------------- */

esp_err_t research_api_register(httpd_handle_t server, GlobalState *state)
{
    s_state = state;
    s_ready = state != NULL;

    research_guard_config_t cfg = configured_guard();
    if (research_guard_init(&s_guard, &cfg) != RESEARCH_GUARD_OK) {
        ESP_LOGE(TAG, "research guard configuration rejected; the API stays disarmed");
        memset(&s_guard, 0, sizeof(s_guard));
        s_guard.state = RESEARCH_GUARD_DISARMED;
        s_guard.last_refusal = RESEARCH_GUARD_ERR_ARG;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/api/research/status", .method = HTTP_GET, .handler = GET_research_status, .user_ctx = NULL },
        { .uri = "/api/research/capabilities", .method = HTTP_GET, .handler = GET_research_capabilities, .user_ctx = NULL },
        { .uri = "/api/research/challenge", .method = HTTP_POST, .handler = POST_research_challenge, .user_ctx = NULL },
        { .uri = "/api/research/arm", .method = HTTP_POST, .handler = POST_research_arm, .user_ctx = NULL },
        { .uri = "/api/research/pause", .method = HTTP_POST, .handler = POST_research_pause, .user_ctx = NULL },
        { .uri = "/api/research/resume", .method = HTTP_POST, .handler = POST_research_resume, .user_ctx = NULL },
        { .uri = "/api/research/cancel", .method = HTTP_POST, .handler = POST_research_cancel, .user_ctx = NULL },
        { .uri = "/api/research/disarm", .method = HTTP_POST, .handler = POST_research_disarm, .user_ctx = NULL },
        { .uri = "/api/research/sha256", .method = HTTP_POST, .handler = POST_research_sha256, .user_ctx = NULL },
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "registering %s failed: %s", uris[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    ESP_LOGW(TAG, "SHA research API enabled: %u jobs, %u s, ceiling %d C",
             (unsigned) s_guard.cfg.max_jobs, (unsigned) (s_guard.cfg.max_runtime_ms / 1000ull),
             CONFIG_TIMSMINER_SHA_RESEARCH_TEMPERATURE_C);
    return ESP_OK;
}

#else /* !CONFIG_TIMSMINER_SHA_RESEARCH */

esp_err_t research_api_register(httpd_handle_t server, GlobalState *state)
{
    (void) server;
    (void) state;
    return ESP_OK;
}

#endif /* CONFIG_TIMSMINER_SHA_RESEARCH */
