/*
 * Copyright 2026 MacEntyre
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

/* Unit tests for auth_check_token_string() - the pure comparison at the
 * core of apibridge's bearer-token auth. auth_check_request() itself needs
 * a live struct mg_connection and is exercised end to end instead by
 * apibridge/tools/component-test.py.
 *
 * auth_check_write_request() shares that same comparison logic (via the
 * internal token_matches() helper) against g_config.write_token instead of
 * g_config.token, so its request-parsing path is covered the same way, by
 * component-test.py. The one behavior unique to it - and pure enough to
 * test here - is that it short-circuits to false without even looking at
 * the connection when no write token is configured (control not enabled),
 * which is exactly the 501-vs-401 distinction httpapi.c relies on. */

#include "apibridge.h"
#include "auth.h"
#include "test_util.h"

struct apibridge_config g_config;

int main(void)
{
	g_config.token = "s3cr3t-token";

	CHECK("matching token accepted", auth_check_token_string("s3cr3t-token"), true);
	CHECK("wrong token rejected", auth_check_token_string("wrong-token-"), false);
	CHECK("shorter token rejected", auth_check_token_string("s3cr3t"), false);
	CHECK("longer token rejected", auth_check_token_string("s3cr3t-token-extra"), false);
	CHECK("empty presented rejected", auth_check_token_string(""), false);
	CHECK("NULL presented rejected", auth_check_token_string(NULL), false);

	g_config.token = NULL;
	CHECK("NULL configured token rejected", auth_check_token_string("anything"), false);

	g_config.write_token = NULL;
	CHECK("unset write token short-circuits without touching conn",
	      auth_check_write_request(NULL), false);

	TEST_EXIT();
}
