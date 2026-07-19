#ifndef __APIBRIDGE_AUTH_H__
#define __APIBRIDGE_AUTH_H__

#include <stdbool.h>
#include <civetweb.h>

/* Compares presented against g_config.token. Not constant-time beyond the
 * matching length prefix - acceptable for a local/LAN tool, not a
 * general-purpose auth server (see Phase 2/3 plan for hardening). */
bool auth_check_token_string(const char *presented);

/* Checks "Authorization: Bearer <token>" first, falling back to a "?token="
 * query-string parameter (needed because mobile WebSocket clients can't
 * always set custom headers on the upgrade handshake). */
bool auth_check_request(const struct mg_connection *conn);

/* Same shape as auth_check_request(), but checks against g_config.write_token
 * instead. Always false if write_token is unset (control not enabled by
 * cgminer) - callers should prefer returning 501 in that case, see
 * control.c, rather than treating it as a plain 401. */
bool auth_check_write_request(const struct mg_connection *conn);

#endif /* __APIBRIDGE_AUTH_H__ */
