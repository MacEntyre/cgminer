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

static pthread_t poll_thread_id;
static volatile bool poll_running;
static statscache_update_cb update_cb;

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
	return NULL;
}

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

		if (update_cb) {
			json_t *envelope = statscache_get_envelope(NULL);

			update_cb(envelope);
			json_decref(envelope);
		}

		usleep((useconds_t)g_config.poll_interval_ms * 1000);
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
	poll_running = false;
	pthread_join(poll_thread_id, NULL);
}
