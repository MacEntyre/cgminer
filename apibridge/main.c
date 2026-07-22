/*
 * Copyright 2026 MacEntyre
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

/* apibridge: a standalone companion process to cgminer. Polls cgminer's
 * local socket API (api.c, one JSON request-response per connection) and
 * re-exposes it as a modern HTTP/WebSocket JSON API for a mobile dashboard.
 * Deliberately a separate process/binary, fork/exec'd and supervised by
 * cgminer (see cgminer-apibridge.c) - a bug here must never be able to take
 * the mining daemon down with it. */

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <civetweb.h>
#include <jansson.h>

#include "apibridge.h"
#include "control.h"
#include "settings.h"
#include "httpapi.h"
#include "statscache.h"
#include "wsapi.h"

struct apibridge_config g_config;

/* apibridge's stderr is inherited straight from cgminer (see
 * cgminer-apibridge.c) and interleaves with cgminer's own applog() lines in
 * the same terminal/log stream, so the timestamp format here matches
 * cgminer's get_datestamp() (cgminer.c) - including millisecond precision -
 * rather than drifting into a visibly different style. */
void bridge_log(const char *fmt, ...)
{
	struct timeval now;
	struct tm tm_now;
	char timebuf[32];
	va_list ap;

	gettimeofday(&now, NULL);
	localtime_r(&now.tv_sec, &tm_now);
	snprintf(timebuf, sizeof(timebuf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
		 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
		 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
		 (int)(now.tv_usec / 1000));

	/* cgminer's own applog() (logging.c) leads with a space before the
	 * bracket; match it so apibridge's lines don't sit one column left
	 * of cgminer's in the interleaved stderr stream. */
	fprintf(stderr, " [%s] apibridge: ", timebuf);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	fflush(stderr);
}

static volatile sig_atomic_t shutdown_requested;

static void handle_signal(int sig)
{
	(void)sig;
	shutdown_requested = 1;
}

static void ws_broadcast_cb(json_t *envelope)
{
	wsapi_broadcast(envelope);
}

int main(int argc, char **argv)
{
	struct mg_context *ctx;
	const char *mg_options[10];
	int opt_i = 0;
	char port_buf[64];

	config_parse_args(argc, argv, &g_config);

	if (g_config.write_token) {
		/* One-shot at startup, not re-checked per request: this is a
		 * static ACL config on cgminer's side (--api-allow), not
		 * something that flaps. A blocking call here is fine - cgminer
		 * only spawns apibridge once its own API socket is already
		 * confirmed listening. */
		bool available = control_check_privileged();

		control_set_available(available);
		if (!available) {
			bridge_log("control endpoints enabled but cgminer denied the 'privileged' check - "
				   "start cgminer with --api-allow granting W to 127.0.0.1, e.g. "
				   "--api-allow W:127.0.0.1, or all control requests will be rejected");
		}
	}

	signal(SIGTERM, handle_signal);
	signal(SIGINT, handle_signal);
	signal(SIGPIPE, SIG_IGN);

	snprintf(port_buf, sizeof(port_buf), "%s:%d", g_config.listen_bind, g_config.listen_port);

	/* civetweb copies option strings internally during mg_start(), so a
	 * function-local buffer for port_buf is fine here. No document_root
	 * is served (points at a path that can't exist as a directory) to
	 * keep the exposed surface limited to the explicitly registered
	 * "/api/v1/" handlers. */
	mg_options[opt_i++] = "listening_ports";
	mg_options[opt_i++] = port_buf;
	mg_options[opt_i++] = "num_threads";
	mg_options[opt_i++] = "4";
	mg_options[opt_i++] = "document_root";
	mg_options[opt_i++] = "/nonexistent";
	mg_options[opt_i++] = "enable_directory_listing";
	mg_options[opt_i++] = "no";
	mg_options[opt_i++] = NULL;

	mg_init_library(MG_FEATURES_WEBSOCKET | MG_FEATURES_IPV6);
	ctx = mg_start(NULL, NULL, mg_options);
	if (!ctx) {
		bridge_log("failed to start HTTP listener on %s", port_buf);
		return 1;
	}

	httpapi_register(ctx);
	wsapi_register(ctx);

	statscache_set_update_callback(ws_broadcast_cb);
	statscache_start();

	bridge_log("listening on %s, proxying cgminer at %s:%d",
		   port_buf, g_config.cgminer_host, g_config.cgminer_port);

	while (!shutdown_requested)
		sleep(1);

	bridge_log("shutting down");
	statscache_stop();
	mg_stop(ctx);
	mg_exit_library();

	return 0;
}
