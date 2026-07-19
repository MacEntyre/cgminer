/*
 * Copyright 2026 MacEntyre
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <civetweb.h>
#include <jansson.h>

#include "apibridge.h"
#include "auth.h"
#include "statscache.h"
#include "wsapi.h"

#define WSAPI_MAX_CLIENTS 32

static struct mg_connection *clients[WSAPI_MAX_CLIENTS];
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;

static int connect_handler(const struct mg_connection *conn, void *cbdata)
{
	(void)cbdata;

	if (!auth_check_request(conn)) {
		bridge_log("wsapi: rejected unauthenticated websocket connect");
		return 1; /* reject */
	}
	return 0; /* accept */
}

static void send_envelope(struct mg_connection *conn, json_t *envelope)
{
	char *dumped = json_dumps(envelope, JSON_COMPACT);

	if (!dumped)
		return;
	mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, dumped, strlen(dumped));
	free(dumped);
}

static void ready_handler(struct mg_connection *conn, void *cbdata)
{
	int i;
	json_t *envelope;

	(void)cbdata;

	pthread_mutex_lock(&clients_lock);
	for (i = 0; i < WSAPI_MAX_CLIENTS; i++) {
		if (!clients[i]) {
			clients[i] = conn;
			break;
		}
	}
	pthread_mutex_unlock(&clients_lock);

	if (i == WSAPI_MAX_CLIENTS)
		bridge_log("wsapi: client table full (%d), connection not tracked for broadcast", WSAPI_MAX_CLIENTS);

	/* Push the current snapshot immediately so the client doesn't wait a
	 * full poll cycle for its first update. */
	envelope = statscache_get_envelope(NULL);
	send_envelope(conn, envelope);
	json_decref(envelope);
}

static int data_handler(struct mg_connection *conn, int bits, char *data, size_t data_len, void *cbdata)
{
	(void)conn;
	(void)bits;
	(void)data;
	(void)data_len;
	(void)cbdata;

	/* Phase 1 is read-only: ignore any client->server payloads (civetweb
	 * handles ping/pong internally) but keep the connection open. */
	return 1;
}

static void close_handler(const struct mg_connection *conn, void *cbdata)
{
	int i;

	(void)cbdata;

	pthread_mutex_lock(&clients_lock);
	for (i = 0; i < WSAPI_MAX_CLIENTS; i++) {
		if (clients[i] == (const struct mg_connection *)conn) {
			clients[i] = NULL;
			break;
		}
	}
	pthread_mutex_unlock(&clients_lock);
}

void wsapi_broadcast(json_t *envelope)
{
	char *dumped = json_dumps(envelope, JSON_COMPACT);
	int i;

	if (!dumped)
		return;

	pthread_mutex_lock(&clients_lock);
	for (i = 0; i < WSAPI_MAX_CLIENTS; i++) {
		if (clients[i])
			mg_websocket_write(clients[i], MG_WEBSOCKET_OPCODE_TEXT, dumped, strlen(dumped));
	}
	pthread_mutex_unlock(&clients_lock);

	free(dumped);
}

void wsapi_register(struct mg_context *ctx)
{
	mg_set_websocket_handler(ctx, "/api/v1/stream",
				  connect_handler, ready_handler, data_handler, close_handler,
				  NULL);
}
