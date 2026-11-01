#ifndef HTTP_SERVER_H_
#define HTTP_SERVER_H_

#include <esp_http_server.h>

#include "cJSON.h"

typedef struct GlobalState GlobalState;

esp_err_t is_network_allowed(httpd_req_t * req);
esp_err_t set_cors_headers(httpd_req_t * req);
esp_err_t start_rest_server(GlobalState * GLOBAL_STATE);
esp_err_t HTTP_send_json(httpd_req_t * req, const cJSON * item, int * prebuffer_len);

/* Sets the HTTP status, CORS headers and a {"status":..,"message":..} body.
 * Shared with the research API, which must answer with the same shape as the
 * rest of the system API. */
esp_err_t HTTP_send_json_error(httpd_req_t * req, const char * status, const char * message);

#endif /* HTTP_SERVER_H_ */
