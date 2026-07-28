/*
 * Copyright 2026 MacEntyre
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <civetweb.h>
#include <jansson.h>

#include "apibridge.h"
#include "auth.h"
#include "control.h"
#include "httpapi.h"
#include "statscache.h"

/* openapi_spec_yaml, docs_ui_html, redoc_standalone_js (+ *_len) - the
 * spec, its Redoc HTML shell, and the vendored Redoc bundle, compiled in
 * at build time by tools/embed_file.sh (see main.c's document_root
 * comment for why these are served through dedicated handlers below
 * instead of civetweb's static file serving). */
#include "generated/docs_embed.h"

/* Control POST bodies are tiny ({"asc_id":0,"option":"freq","value":650}) -
 * bound the read rather than growing a buffer, and reject anything larger
 * up front via Content-Length instead of silently truncating. */
#define CONTROL_BODY_MAX 512

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

static int send_error(struct mg_connection *conn, int status_code, const char *error)
{
	json_t *body = json_object();

	json_object_set_new(body, "error", json_string(error));
	send_json(conn, status_code, body);
	json_decref(body);
	return status_code;
}

static const char *remote_addr_of(struct mg_connection *conn)
{
	const struct mg_request_info *ri = mg_get_request_info(conn);

	return (ri && ri->remote_addr[0]) ? ri->remote_addr : "?";
}

/* Shared body for /api/v1/control and /api/v1/control/reset: enforce POST,
 * the write token, and a bounded JSON body, then hand the parsed object to
 * fn. fn takes ownership of req_out via a returned HTTP status code; this
 * wrapper always decrefs it after fn returns. */
static int handle_control_request(struct mg_connection *conn,
				   int (*fn)(struct mg_connection *, json_t *, const char *))
{
	const struct mg_request_info *ri = mg_get_request_info(conn);
	char body[CONTROL_BODY_MAX];
	json_t *req;
	json_error_t jerr;
	int n, status;
	const char *remote = remote_addr_of(conn);

	if (!g_config.write_token) {
		bridge_log("AUDIT: control request from %s -> 501 (control not enabled)", remote);
		return send_error(conn, 501, "control endpoints not enabled");
	}

	if (!auth_check_write_request(conn)) {
		bridge_log("AUDIT: control request from %s -> unauthorized", remote);
		send_unauthorized(conn);
		return 401;
	}

	if (!ri || strcmp(ri->request_method, "POST") != 0)
		return send_error(conn, 405, "POST required");

	if (ri->content_length <= 0 || ri->content_length >= (long long)sizeof(body))
		return send_error(conn, 400, "request body missing or too large");

	n = mg_read(conn, body, (size_t)ri->content_length);
	if (n < 0 || (long long)n != ri->content_length)
		return send_error(conn, 400, "failed to read request body");
	body[n] = '\0';

	req = json_loadb(body, (size_t)n, 0, &jerr);
	if (!req || !json_is_object(req)) {
		if (req)
			json_decref(req);
		bridge_log("AUDIT: control request from %s -> 400 (invalid JSON body)", remote);
		return send_error(conn, 400, "invalid JSON body");
	}

	status = fn(conn, req, remote);
	json_decref(req);
	return status;
}

/* Serves one of the embedded docs assets verbatim (byte-exact, no
 * transcoding) with the given content type. No auth: these are static
 * documentation with no operational data, same tier as /api/v1/health -
 * gating them would defeat the point of a spec that's browsable by
 * third parties. */
static int send_embedded(struct mg_connection *conn, const char *content_type,
			  const unsigned char *data, size_t len)
{
	mg_printf(conn,
		  "HTTP/1.1 200 OK\r\n"
		  "Content-Type: %s\r\n"
		  "Content-Length: %lu\r\n"
		  "Connection: close\r\n\r\n",
		  content_type, (unsigned long)len);
	mg_write(conn, data, len);
	return 200;
}

static int handler_openapi_spec(struct mg_connection *conn, void *cbdata)
{
	(void)cbdata;
	return send_embedded(conn, "application/yaml", openapi_spec_yaml, openapi_spec_yaml_len);
}

static int handler_docs_ui(struct mg_connection *conn, void *cbdata)
{
	(void)cbdata;
	return send_embedded(conn, "text/html", docs_ui_html, docs_ui_html_len);
}

static int handler_docs_redoc_js(struct mg_connection *conn, void *cbdata)
{
	(void)cbdata;
	return send_embedded(conn, "application/javascript", redoc_standalone_js, redoc_standalone_js_len);
}

static bool get_asc_id(json_t *req, int *asc_id_out)
{
	json_t *v = json_object_get(req, "asc_id");

	if (!json_is_integer(v))
		return false;
	*asc_id_out = (int)json_integer_value(v);
	return *asc_id_out >= 0;
}

/* Relays resp (transferring ownership) to the client per
 * control_status_to_http(), audit-logs the outcome, and returns the HTTP
 * status used. resp may be NULL (cgminer unreachable / bad response). */
static int relay_control_response(struct mg_connection *conn, json_t *resp, const char *remote,
				   int asc_id, const char *option, const char *value_desc)
{
	const char *msg = NULL;
	int code;
	json_t *out;

	if (!resp) {
		bridge_log("AUDIT: control asc_id=%d option=%s value=%s from=%s -> 502 (cgminer unreachable)",
			   asc_id, option, value_desc, remote);
		return send_error(conn, 502, "cgminer unreachable");
	}

	code = control_status_to_http(resp, &msg);

	out = json_object();
	json_object_set_new(out, "message", msg ? json_string(msg) : json_null());
	send_json(conn, code, out);
	json_decref(out);

	bridge_log("AUDIT: control asc_id=%d option=%s value=%s from=%s -> %d %s",
		   asc_id, option, value_desc, remote, code, msg ? msg : "");

	json_decref(resp);
	return code;
}

static int do_control(struct mg_connection *conn, json_t *req, const char *remote)
{
	int asc_id;
	json_t *opt_v, *val_v;
	const char *option;
	bool needs_value, value_present = false;
	double value = 0;
	char option_buf[32];
	char value_desc[32] = "-";
	json_t *resp;

	if (!get_asc_id(req, &asc_id))
		return send_error(conn, 400, "missing/invalid asc_id");

	opt_v = json_object_get(req, "option");
	option = json_is_string(opt_v) ? json_string_value(opt_v) : NULL;
	if (!option || !control_option_allowed(option, &needs_value)) {
		bridge_log("AUDIT: control asc_id=%d option=%s from=%s -> 400 (not whitelisted)",
			   asc_id, option ? option : "?", remote);
		return send_error(conn, 400, "option not allowed");
	}
	snprintf(option_buf, sizeof(option_buf), "%s", option);

	if (needs_value) {
		val_v = json_object_get(req, "value");
		if (!json_is_number(val_v))
			return send_error(conn, 400, "missing/invalid value");
		value = json_number_value(val_v);
		value_present = true;
		snprintf(value_desc, sizeof(value_desc), "%g", value);
	}

	resp = control_relay_ascset(asc_id, option_buf, value_present, value);
	return relay_control_response(conn, resp, remote, asc_id, option_buf, value_desc);
}

static int do_control_reset(struct mg_connection *conn, json_t *req, const char *remote)
{
	int asc_id;
	json_t *resp;

	if (!get_asc_id(req, &asc_id))
		return send_error(conn, 400, "missing/invalid asc_id");

	resp = control_relay_reset(asc_id);
	return relay_control_response(conn, resp, remote, asc_id, "reset", "-");
}

static int handler_control(struct mg_connection *conn, void *cbdata)
{
	(void)cbdata;
	return handle_control_request(conn, do_control);
}

static int handler_control_reset(struct mg_connection *conn, void *cbdata)
{
	(void)cbdata;
	return handle_control_request(conn, do_control_reset);
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
	json_object_set_new(body, "control_enabled", json_boolean(g_config.write_token != NULL));
	json_object_set_new(body, "control_available", json_boolean(control_is_available()));

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
	mg_set_request_handler(ctx, "/api/v1/control", handler_control, NULL);
	mg_set_request_handler(ctx, "/api/v1/control/reset", handler_control_reset, NULL);
	mg_set_request_handler(ctx, "/openapi.yaml", handler_openapi_spec, NULL);
	mg_set_request_handler(ctx, "/docs", handler_docs_ui, NULL);
	mg_set_request_handler(ctx, "/docs/redoc.standalone.js", handler_docs_redoc_js, NULL);
}
