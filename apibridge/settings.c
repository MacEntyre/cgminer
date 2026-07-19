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

#include "settings.h"

static void usage_and_exit(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [--cgminer-host HOST] [--cgminer-port PORT] [--listen-port PORT] [--poll-interval-ms MS]\n"
		"Requires CGMINER_APIBRIDGE_TOKEN in the environment.\n"
		"Optional: CGMINER_APIBRIDGE_BIND to override the listen bind address (default 0.0.0.0).\n",
		prog);
	exit(1);
}

void config_parse_args(int argc, char **argv, struct apibridge_config *cfg)
{
	int i;

	cfg->cgminer_host = "127.0.0.1";
	cfg->cgminer_port = 4028;
	cfg->listen_port = 4029;
	cfg->poll_interval_ms = 2000;
	cfg->listen_bind = getenv("CGMINER_APIBRIDGE_BIND");
	if (!cfg->listen_bind)
		cfg->listen_bind = "0.0.0.0";
	cfg->token = getenv("CGMINER_APIBRIDGE_TOKEN");

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--cgminer-host") && i + 1 < argc)
			cfg->cgminer_host = argv[++i];
		else if (!strcmp(argv[i], "--cgminer-port") && i + 1 < argc)
			cfg->cgminer_port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--listen-port") && i + 1 < argc)
			cfg->listen_port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--poll-interval-ms") && i + 1 < argc)
			cfg->poll_interval_ms = atoi(argv[++i]);
		else
			usage_and_exit(argv[0]);
	}

	if (!cfg->token || !*cfg->token) {
		fprintf(stderr, "%s: CGMINER_APIBRIDGE_TOKEN is required in the environment\n", argv[0]);
		exit(1);
	}
}
