/*
 * Copyright 2026 MacEntyre
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

/* Unit tests for the pure logic in control.c: the option whitelist, wire
 * parameter construction, and the STATUS-to-HTTP mapping. The actual relay
 * calls (control_relay_ascset/control_relay_reset/control_check_privileged)
 * go over the network via cgclient and are exercised end to end instead by
 * apibridge/tools/component-test.py - here cgclient_query[_param]() are
 * stubbed out (unused) so this binary doesn't need to link the real
 * cgclient.c or civetweb. */

#include <string.h>

#include "apibridge.h"
#include "control.h"
#include "test_util.h"

struct apibridge_config g_config;

json_t *cgclient_query(const char *host, int port, const char *command, int timeout_s)
{
	(void)host;
	(void)port;
	(void)command;
	(void)timeout_s;
	return NULL;
}

json_t *cgclient_query_param(const char *host, int port, const char *command, const char *parameter,
			      int timeout_s)
{
	(void)host;
	(void)port;
	(void)command;
	(void)parameter;
	(void)timeout_s;
	return NULL;
}

static void test_option_whitelist(void)
{
	bool needs_value;

	CHECK("freq allowed", control_option_allowed("freq", &needs_value), true);
	CHECK("freq needs value", needs_value, true);

	CHECK("case-insensitive FREQ allowed", control_option_allowed("FREQ", &needs_value), true);

	CHECK("target allowed", control_option_allowed("target", &needs_value), true);
	CHECK("target needs value", needs_value, true);

	CHECK("corev allowed", control_option_allowed("corev", &needs_value), true);
	CHECK("corev needs value", needs_value, true);

	CHECK("setfan allowed", control_option_allowed("setfan", &needs_value), true);
	CHECK("setfan needs value", needs_value, true);

	CHECK("lockfreq allowed", control_option_allowed("lockfreq", &needs_value), true);
	CHECK("lockfreq does not need value", needs_value, false);

	CHECK("unlockfreq allowed", control_option_allowed("unlockfreq", &needs_value), true);
	CHECK("unlockfreq does not need value", needs_value, false);

	CHECK("zeromaxt allowed", control_option_allowed("zeromaxt", &needs_value), true);
	CHECK("zeromaxt does not need value", needs_value, false);

	/* reset is deliberately not on the generic whitelist - it has its own
	 * dedicated endpoint (/api/v1/control/reset) so a typo'd "option"
	 * string can't reach it. */
	CHECK("reset not allowed via generic whitelist", control_option_allowed("reset", &needs_value), false);
	CHECK("chip:freq not allowed (v1 scope)", control_option_allowed("chip:freq", &needs_value), false);
	CHECK("waitfactor not allowed (v1 scope)", control_option_allowed("waitfactor", &needs_value), false);
	CHECK("usbprop not allowed (v1 scope)", control_option_allowed("usbprop", &needs_value), false);
	CHECK("require not allowed (v1 scope)", control_option_allowed("require", &needs_value), false);
	CHECK("unknown option not allowed", control_option_allowed("bogus", &needs_value), false);
	CHECK("empty option not allowed", control_option_allowed("", &needs_value), false);
	CHECK("NULL option not allowed", control_option_allowed(NULL, &needs_value), false);
}

static void test_build_parameter(void)
{
	char buf[CONTROL_PARAM_MAX];
	char tiny[4];

	CHECK("freq with value builds", control_build_parameter(buf, sizeof(buf), 0, "freq", true, 650), true);
	CHECK_STR("freq with value parameter", buf, "0,freq,650");

	CHECK("lockfreq without value builds", control_build_parameter(buf, sizeof(buf), 1, "lockfreq", false, 0), true);
	CHECK_STR("lockfreq without value parameter", buf, "1,lockfreq");

	CHECK("corev with fractional value builds", control_build_parameter(buf, sizeof(buf), 2, "corev", true, 250.5), true);
	CHECK_STR("corev fractional parameter", buf, "2,corev,250.5");

	CHECK("negative asc_id rejected", control_build_parameter(buf, sizeof(buf), -1, "freq", true, 650), false);
	CHECK("empty option rejected", control_build_parameter(buf, sizeof(buf), 0, "", true, 650), false);
	CHECK("NULL option rejected", control_build_parameter(buf, sizeof(buf), 0, NULL, true, 650), false);
	CHECK("undersized buffer rejected", control_build_parameter(tiny, sizeof(tiny), 0, "freq", true, 650), false);
}

static json_t *make_status(const char *status, const char *msg)
{
	json_t *entry = json_object();
	json_t *arr = json_array();
	json_t *root = json_object();

	json_object_set_new(entry, "STATUS", json_string(status));
	if (msg)
		json_object_set_new(entry, "Msg", json_string(msg));
	json_array_append_new(arr, entry);
	json_object_set_new(root, "STATUS", arr);

	return root;
}

static void test_status_to_http(void)
{
	json_t *resp;
	const char *msg;

	resp = make_status("S", "ASC 0 set OK");
	CHECK("success status maps to 200", control_status_to_http(resp, &msg), 200);
	CHECK_STR("success message passed through", msg, "ASC 0 set OK");
	json_decref(resp);

	resp = make_status("E", "Access denied to 'ascset' command");
	CHECK("access denied maps to 403", control_status_to_http(resp, &msg), 403);
	json_decref(resp);

	resp = make_status("E", "invalid chip 5");
	CHECK("other driver error maps to 422", control_status_to_http(resp, &msg), 422);
	CHECK_STR("driver error message passed through", msg, "invalid chip 5");
	json_decref(resp);

	resp = json_object();
	json_object_set_new(resp, "STATUS", json_array());
	CHECK("empty STATUS array maps to 502", control_status_to_http(resp, NULL), 502);
	json_decref(resp);

	resp = json_object();
	CHECK("missing STATUS key maps to 502", control_status_to_http(resp, NULL), 502);
	json_decref(resp);

	CHECK("NULL response maps to 502", control_status_to_http(NULL, NULL), 502);
}

static void test_availability_cache(void)
{
	CHECK("availability defaults to false", control_is_available(), false);

	control_set_available(true);
	CHECK("availability true after set", control_is_available(), true);

	control_set_available(false);
	CHECK("availability false after unset", control_is_available(), false);
}

int main(void)
{
	test_option_whitelist();
	test_build_parameter();
	test_status_to_http();
	test_availability_cache();

	TEST_EXIT();
}
