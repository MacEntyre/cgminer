/*
 * Copyright 2026 MacEntyre
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <civetweb.h>
#include <jansson.h>

#include "apibridge.h"
#include "auth.h"
#include "httpapi.h"
#include "statscache.h"

static void send_json(struct mg_connection *conn, int status_code, json_t *body)
{
	char *dumped = json_dumps(body, JSON_COMPACT);
	size_t len = dumped ? strlen(dumped) : 0;

	mg_printf(conn,
		  "HTTP/1.1 %d OK\r\n"
		  "Content-Type: application/json\r\n"
		  "Content-Length: %lu\r\n"
		  "Connection: close\r\n\r\n",
		  status_code, (unsigned long)len);
	if (dumped) {
		mg_write(conn, dumped, len);
		free(dumped);
	}
}

static void send_unauthorized(struct mg_connection *conn)
{
	json_t *body = json_object();

	json_object_set_new(body, "error", json_string("unauthorized"));
	send_json(conn, 401, body);
	json_decref(body);
}

static int handle_stat(struct mg_connection *conn, const char *key)
{
	bool stale;
	double age_s;
	json_t *data;
	json_t *body;

	if (!auth_check_request(conn)) {
		send_unauthorized(conn);
		return 401;
	}

	data = statscache_get(key, &stale, &age_s);
	body = json_object();

	if (data)
		json_object_set_new(body, key, data);
	else
		json_object_set_new(body, key, json_null());
	json_object_set_new(body, "stale", json_boolean(stale || !data));
	json_object_set_new(body, "age_s", data ? json_real(age_s) : json_null());

	send_json(conn, 200, body);
	json_decref(body);
	return 200;
}

static int handler_summary(struct mg_connection *conn, void *cbdata)
{
	(void)cbdata;
	return handle_stat(conn, "summary");
}

static int handler_devs(struct mg_connection *conn, void *cbdata)
{
	(void)cbdata;
	return handle_stat(conn, "devs");
}

static int handler_pools(struct mg_connection *conn, void *cbdata)
{
	(void)cbdata;
	return handle_stat(conn, "pools");
}

static int handler_stats(struct mg_connection *conn, void *cbdata)
{
	(void)cbdata;
	return handle_stat(conn, "stats");
}

static int handler_health(struct mg_connection *conn, void *cbdata)
{
	bool stale;
	double age_s;
	json_t *summary;
	json_t *body = json_object();

	(void)cbdata;

	summary = statscache_get("summary", &stale, &age_s);

	json_object_set_new(body, "bridge", json_string("ok"));
	json_object_set_new(body, "cgminer_reachable", json_boolean(summary && !stale));
	json_object_set_new(body, "last_updated_age_s", summary ? json_real(age_s) : json_null());

	if (summary)
		json_decref(summary);

	send_json(conn, 200, body);
	json_decref(body);
	return 200;
}

void httpapi_register(struct mg_context *ctx)
{
	mg_set_request_handler(ctx, "/api/v1/summary", handler_summary, NULL);
	mg_set_request_handler(ctx, "/api/v1/devs", handler_devs, NULL);
	mg_set_request_handler(ctx, "/api/v1/pools", handler_pools, NULL);
	mg_set_request_handler(ctx, "/api/v1/stats", handler_stats, NULL);
	mg_set_request_handler(ctx, "/api/v1/health", handler_health, NULL);
}
