#ifndef __APIBRIDGE_CGCLIENT_H__
#define __APIBRIDGE_CGCLIENT_H__

#include <jansson.h>

/* Sends {"command":"<command>"} to cgminer's local socket API at host:port
 * and returns the parsed JSON response (caller must json_decref()), or NULL
 * on any connect/timeout/parse failure. Matches cgminer's api.c wire format:
 * one JSON request per connection, sent in a single write(), response read
 * until the server closes the socket (no length/delimiter framing). */
json_t *cgclient_query(const char *host, int port, const char *command, int timeout_s);

#endif /* __APIBRIDGE_CGCLIENT_H__ */
