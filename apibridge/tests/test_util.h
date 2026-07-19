/*
 * Copyright 2026 MacEntyre
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

/* Minimal assert-and-print harness shared by the apibridge unit tests.
 * Deliberately not a real framework (no cmocka/Unity dependency) - a couple
 * of pure-function test binaries don't need one. */

#ifndef __APIBRIDGE_TEST_UTIL_H__
#define __APIBRIDGE_TEST_UTIL_H__

#include <stdio.h>
#include <string.h>

static int test_failures;

#define CHECK(desc, got, want) do { \
	long _g = (long)(got); \
	long _w = (long)(want); \
	if (_g == _w) { \
		printf("OK   %s\n", desc); \
	} else { \
		printf("FAIL %s (got %ld, want %ld)\n", desc, _g, _w); \
		test_failures++; \
	} \
} while (0)

#define CHECK_STR(desc, got, want) do { \
	const char *_g = (got); \
	const char *_w = (want); \
	if (_g && _w && strcmp(_g, _w) == 0) { \
		printf("OK   %s\n", desc); \
	} else { \
		printf("FAIL %s (got %s, want %s)\n", desc, _g ? _g : "(null)", _w ? _w : "(null)"); \
		test_failures++; \
	} \
} while (0)

#define TEST_EXIT() return test_failures ? 1 : 0

#endif /* __APIBRIDGE_TEST_UTIL_H__ */
