#ifndef __APIBRIDGE_STATSCACHE_H__
#define __APIBRIDGE_STATSCACHE_H__

#include <stdbool.h>
#include <jansson.h>

/* Called from the poll thread every cycle with a fresh envelope (borrowed
 * reference - do not decref, do not hold a pointer past the callback). */
typedef void (*statscache_update_cb)(json_t *envelope);

void statscache_set_update_callback(statscache_update_cb cb);

/* Spawns the poll thread (summary/devs/pools against cgminer's socket API,
 * per g_config.poll_interval_ms). */
void statscache_start(void);
void statscache_stop(void);

/* Returns a new reference to the last-known-good response for "summary",
 * "devs", "pools", "stats", "version", "config", "coin" or "notify" (the
 * raw cgminer JSON, STATUS wrapper included), or NULL if never successfully
 * polled yet. *stale is set true if cgminer was unreachable on the most
 * recent poll (the returned data, if any, is then the last-known-good
 * snapshot, not fresh). */
json_t *statscache_get(const char *key, bool *stale, double *age_s);

/* Combined {"type":"stats","summary":...,"devs":...,"pools":...,"stale":...,
 * "ts":...} envelope, new reference, caller must json_decref(). */
json_t *statscache_get_envelope(bool *stale);

#endif /* __APIBRIDGE_STATSCACHE_H__ */
