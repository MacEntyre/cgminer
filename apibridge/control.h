#ifndef __APIBRIDGE_CONTROL_H__
#define __APIBRIDGE_CONTROL_H__

#include <stddef.h>
#include <stdbool.h>
#include <jansson.h>

/* Wire parameter for ascset never exceeds "<asc_id>,<option>,<value>" -
 * asc_id/value are apibridge-constructed numbers, option is a whitelisted
 * name, so this is comfortably an upper bound. */
#define CONTROL_PARAM_MAX 96

/* Whether option is on the Phase 2 control whitelist and, if so, whether it
 * takes a numeric value (true) or none (false, e.g. lockfreq/zeromaxt).
 * *needs_value is only set when this returns true. Case-insensitive, same
 * as cgminer's own compac_api_set(). */
bool control_option_allowed(const char *option, bool *needs_value);

/* Builds the ascset wire parameter "<asc_id>,<option>[,<value>]" into out.
 * value is re-serialized with "%g", never copied from raw client text, so
 * the constructed parameter can only ever contain [-0-9.eE,] plus the
 * whitelisted option name - no JSON-string escaping is needed downstream in
 * cgclient_query_param(). Returns false if asc_id is negative, option is
 * empty, or out is too small. */
bool control_build_parameter(char *out, size_t out_siz, int asc_id, const char *option,
			      bool value_present, double value);

/* Relays one control command to cgminer's ascset RPC. Returns the parsed
 * cgminer JSON response (caller must json_decref()), or NULL on a
 * transport-level failure (cgminer unreachable, bad JSON, invalid inputs) -
 * not on an ascset-level rejection, which is still a valid parsed response
 * with STATUS "E"; use control_status_to_http() for that. */
json_t *control_relay_ascset(int asc_id, const char *option, bool value_present, double value);

/* Relays a "reset" (device reinit) request for asc_id. Kept separate from
 * control_relay_ascset() so HTTP callers can't reach it through the generic
 * whitelist path (see httpapi.c: /api/v1/control vs
 * /api/v1/control/reset). */
json_t *control_relay_reset(int asc_id);

/* Calls cgminer's "privileged" RPC command, which does nothing but confirm
 * the caller's connection is in cgminer's PRIVGROUP ACL (api.c). Used once
 * at apibridge startup to populate "control_available" on
 * /api/v1/health - see main.c. */
bool control_check_privileged(void);

/* Caches the result of the startup control_check_privileged() call so
 * httpapi.c's /api/v1/health handler can report it without re-querying
 * cgminer on every health check. Defaults to false. */
void control_set_available(bool available);
bool control_is_available(void);

/* Extracts the HTTP status code and human-readable message cgminer's own
 * dispatch chose for a relayed response's first STATUS entry: "S" -> 200,
 * an "Access denied" message -> 403 (missing --api-allow W: rule - see
 * APIBRIDGE-README), any other "E"/"W" -> 422 (the driver's own rejection,
 * e.g. an out-of-range value - relayed verbatim, not re-derived here, per
 * the "relay, don't re-validate" design). Returns 502 if resp isn't a
 * usable api.c-shaped response at all (e.g. cgminer sent something
 * unexpected). resp is a borrowed reference; *msg_out (if non-NULL) points
 * into resp's own strings and is only valid as long as resp is alive. */
int control_status_to_http(json_t *resp, const char **msg_out);

#endif /* __APIBRIDGE_CONTROL_H__ */
