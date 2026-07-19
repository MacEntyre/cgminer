/*
 * Copyright 2026 MacEntyre
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

/* Unit tests for config_parse_args()'s happy paths: defaults, CLI overrides,
 * and the CGMINER_APIBRIDGE_BIND env var. The missing-token and bad-argument
 * paths both exit(1) directly rather than returning an error, so they are
 * left to manual/CLI testing rather than forking a child process here just
 * to observe an exit code. */

#include <stdlib.h>

#include "apibridge.h"
#include "settings.h"
#include "test_util.h"

static void test_defaults(void)
{
	struct apibridge_config cfg;
	char *argv[] = { "apibridged" };

	setenv("CGMINER_APIBRIDGE_TOKEN", "test-token", 1);
	unsetenv("CGMINER_APIBRIDGE_BIND");

	config_parse_args(1, argv, &cfg);

	CHECK_STR("default cgminer_host", cfg.cgminer_host, "127.0.0.1");
	CHECK("default cgminer_port", cfg.cgminer_port, 4028);
	CHECK("default listen_port", cfg.listen_port, 4029);
	CHECK("default poll_interval_ms", cfg.poll_interval_ms, 2000);
	CHECK_STR("default listen_bind", cfg.listen_bind, "0.0.0.0");
	CHECK_STR("token from environment", cfg.token, "test-token");
}

static void test_cli_overrides(void)
{
	struct apibridge_config cfg;
	char *argv[] = {
		"apibridged",
		"--cgminer-host", "192.168.1.50",
		"--cgminer-port", "9999",
		"--listen-port", "5555",
		"--poll-interval-ms", "500",
	};

	setenv("CGMINER_APIBRIDGE_TOKEN", "test-token", 1);

	config_parse_args(9, argv, &cfg);

	CHECK_STR("overridden cgminer_host", cfg.cgminer_host, "192.168.1.50");
	CHECK("overridden cgminer_port", cfg.cgminer_port, 9999);
	CHECK("overridden listen_port", cfg.listen_port, 5555);
	CHECK("overridden poll_interval_ms", cfg.poll_interval_ms, 500);
}

static void test_bind_env_var(void)
{
	struct apibridge_config cfg;
	char *argv[] = { "apibridged" };

	setenv("CGMINER_APIBRIDGE_TOKEN", "test-token", 1);
	setenv("CGMINER_APIBRIDGE_BIND", "127.0.0.1", 1);

	config_parse_args(1, argv, &cfg);

	CHECK_STR("listen_bind from environment", cfg.listen_bind, "127.0.0.1");

	unsetenv("CGMINER_APIBRIDGE_BIND");
}

int main(void)
{
	test_defaults();
	test_cli_overrides();
	test_bind_env_var();

	TEST_EXIT();
}
