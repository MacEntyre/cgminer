#ifndef __APIBRIDGE_WSAPI_H__
#define __APIBRIDGE_WSAPI_H__

#include <civetweb.h>
#include <jansson.h>

/* Registers the /api/v1/stream websocket upgrade handler on ctx. */
void wsapi_register(struct mg_context *ctx);

/* Sends envelope (borrowed reference) to every currently connected
 * websocket client. Called by statscache's poll thread once per cycle. */
void wsapi_broadcast(json_t *envelope);

#endif /* __APIBRIDGE_WSAPI_H__ */
