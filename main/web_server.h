#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Start the HTTP server that serves the configuration UI and the
 * /api/status (GET) and /api/config (POST) endpoints. */
void web_server_start(void);

#ifdef __cplusplus
}
#endif
