#ifndef RESEARCH_API_H_
#define RESEARCH_API_H_

#include <esp_err.h>
#include <esp_http_server.h>

typedef struct GlobalState GlobalState;

/* Registers the /api/research/* handlers.
 *
 * Built only when CONFIG_TIMSMINER_SHA_RESEARCH is set; otherwise the function
 * does nothing and the routes do not exist, so a stock mining build has no
 * research surface at all. The caller does not need an #ifdef.
 *
 * `state` may be NULL only before SYSTEM_init_system(); the handlers answer 503
 * until it is set. */
esp_err_t research_api_register(httpd_handle_t server, GlobalState *state);

#endif /* RESEARCH_API_H_ */
