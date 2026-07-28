/*
 * Copyright 2026 MacEntyre
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "apibridge.h"
#include "cgclient.h"
#include "statscache.h"

struct cache_entry {
	pthread_mutex_t lock;
	json_t *json;
	bool stale;
	time_t last_updated;
};

static struct cache_entry summary_entry = { PTHREAD_MUTEX_INITIALIZER, NULL, true, 0 };
static struct cache_entry devs_entry    = { PTHREAD_MUTEX_INITIALIZER, NULL, true, 0 };
static struct cache_entry pools_entry   = { PTHREAD_MUTEX_INITIALIZER, NULL, true, 0 };
static struct cache_entry stats_entry   = { PTHREAD_MUTEX_INITIALIZER, NULL, true, 0 };
static struct cache_entry version_entry = { PTHREAD_MUTEX_INITIALIZER, NULL, true, 0 };
static struct cache_entry config_entry  = { PTHREAD_MUTEX_INITIALIZER, NULL, true, 0 };
static struct cache_entry coin_entry    = { PTHREAD_MUTEX_INITIALIZER, NULL, true, 0 };
static struct cache_entry notify_entry  = { PTHREAD_MUTEX_INITIALIZER, NULL, true, 0 };

static pthread_t poll_thread_id;
static volatile bool poll_running;
static statscache_update_cb update_cb;

/* Guards poll_running/wake for the poll thread's sleep. A plain usleep()
 * between poll cycles can't be interrupted, so statscache_stop() would have
 * to wait out the full poll interval before pthread_join() returns - racing
 * (and often losing) against cgminer's own APIBRIDGE_STOP_TIMEOUT_MS on
 * SIGTERM and getting SIGKILL'd instead of exiting cleanly. */
static pthread_mutex_t sleep_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sleep_cond = PTHREAD_COND_INITIALIZER;

static void entry_update(struct cache_entry *entry, const char *command)
{
	json_t *resp = cgclient_query(g_config.cgminer_host, g_config.cgminer_port, command, 3);

	pthread_mutex_lock(&entry->lock);
	if (resp) {
		if (entry->json)
			json_decref(entry->json);
		entry->json = resp;
		entry->stale = false;
		entry->last_updated = time(NULL);
	} else {
		/* Keep serving the last-known-good snapshot, just flag it. */
		entry->stale = true;
	}
	pthread_mutex_unlock(&entry->lock);
}

static json_t *entry_get(struct cache_entry *entry, bool *stale, double *age_s)
{
	json_t *ref = NULL;

	pthread_mutex_lock(&entry->lock);
	if (entry->json)
		ref = json_incref(entry->json);
	if (stale)
		*stale = entry->stale;
	if (age_s)
		*age_s = entry->json ? difftime(time(NULL), entry->last_updated) : -1.0;
	pthread_mutex_unlock(&entry->lock);

	return ref;
}

json_t *statscache_get(const char *key, bool *stale, double *age_s)
{
	if (!strcmp(key, "summary"))
		return entry_get(&summary_entry, stale, age_s);
	if (!strcmp(key, "devs"))
		return entry_get(&devs_entry, stale, age_s);
	if (!strcmp(key, "pools"))
		return entry_get(&pools_entry, stale, age_s);
	if (!strcmp(key, "stats"))
		return entry_get(&stats_entry, stale, age_s);
	if (!strcmp(key, "version"))
		return entry_get(&version_entry, stale, age_s);
	if (!strcmp(key, "config"))
		return entry_get(&config_entry, stale, age_s);
	if (!strcmp(key, "coin"))
		return entry_get(&coin_entry, stale, age_s);
	if (!strcmp(key, "notify"))
		return entry_get(&notify_entry, stale, age_s);
	return NULL;
}

/* Deliberately doesn't include the "stats" cache entry (GET /api/v1/stats) -
 * that RPC command's per-device payload is much larger than summary/devs/
 * pools combined, and callers who need it (e.g. Fan/FanCeiling) can poll
 * /api/v1/stats on demand instead of it doubling every /stream push. */
json_t *statscache_get_envelope(bool *stale)
{
	json_t *envelope = json_object();
	bool any_stale = false;
	bool s;
	json_t *v;

	json_object_set_new(envelope, "type", json_string("stats"));

	v = entry_get(&summary_entry, &s, NULL);
	any_stale |= s;
	if (v)
		json_object_set_new(envelope, "summary", v);

	v = entry_get(&devs_entry, &s, NULL);
	any_stale |= s;
	if (v)
		json_object_set_new(envelope, "devs", v);

	v = entry_get(&pools_entry, &s, NULL);
	any_stale |= s;
	if (v)
		json_object_set_new(envelope, "pools", v);

	json_object_set_new(envelope, "stale", json_boolean(any_stale));
	json_object_set_new(envelope, "ts", json_integer((json_int_t)time(NULL)));

	if (stale)
		*stale = any_stale;

	return envelope;
}

void statscache_set_update_callback(statscache_update_cb cb)
{
	update_cb = cb;
}

static void *poll_thread(void *arg)
{
	(void)arg;

	while (poll_running) {
		entry_update(&summary_entry, "summary");
		entry_update(&devs_entry, "devs");
		entry_update(&pools_entry, "pools");
		entry_update(&stats_entry, "stats");
		entry_update(&version_entry, "version");
		entry_update(&config_entry, "config");
		entry_update(&coin_entry, "coin");
		entry_update(&notify_entry, "notify");

		if (update_cb) {
			json_t *envelope = statscache_get_envelope(NULL);

			update_cb(envelope);
			json_decref(envelope);
		}

		pthread_mutex_lock(&sleep_lock);
		if (poll_running) {
			struct timespec deadline;

			clock_gettime(CLOCK_REALTIME, &deadline);
			deadline.tv_sec += g_config.poll_interval_ms / 1000;
			deadline.tv_nsec += (long)(g_config.poll_interval_ms % 1000) * 1000000L;
			if (deadline.tv_nsec >= 1000000000L) {
				deadline.tv_sec++;
				deadline.tv_nsec -= 1000000000L;
			}
			pthread_cond_timedwait(&sleep_cond, &sleep_lock, &deadline);
		}
		pthread_mutex_unlock(&sleep_lock);
	}
	return NULL;
}

void statscache_start(void)
{
	poll_running = true;
	if (pthread_create(&poll_thread_id, NULL, poll_thread, NULL)) {
		bridge_log("statscache: failed to start poll thread");
		poll_running = false;
	}
}

void statscache_stop(void)
{
	if (!poll_running)
		return;
	pthread_mutex_lock(&sleep_lock);
	poll_running = false;
	pthread_cond_signal(&sleep_cond);
	pthread_mutex_unlock(&sleep_lock);
	pthread_join(poll_thread_id, NULL);
}
