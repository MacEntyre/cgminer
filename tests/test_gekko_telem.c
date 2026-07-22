/*
 * Copyright 2025-2026 MacEntyre
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; version 3 of the License.
 * See COPYING for more details.
 */

/* Unit tests for the pure byte<->physical-unit conversions in
 * driver-gekko-telem.c. These have no USB/threading/global-state
 * dependency, so unlike the rest of driver-gekko.c they can be linked into
 * a small standalone binary and tested the same way apibridge/tests does
 * for auth.c/settings.c/control.c. */

#include "driver-gekko-telem.h"
#include "test_util.h"

/* telem_totach() has one applog(LOG_DEBUG, ...) trace call, which pulls in
 * cgminer's global logging state via miner.h/logging.h. Stub just enough
 * (opt_debug stays false, so the applog() macro's own guard skips the body
 * and _applog is never actually called) to link this binary without the
 * rest of cgminer - same approach apibridge/tests/test_control.c uses for
 * stubbing cgclient_query(). */
bool opt_debug;
bool use_syslog;
bool opt_log_output;
int opt_log_level;

void _applog(int prio, const char *str, bool force)
{
	(void)prio;
	(void)str;
	(void)force;
}

static void test_telem_tovin(void)
{
	CHECK_FLOAT("tovin(0) is 0V", telem_tovin(0), 0.0, 0.001);
	CHECK_FLOAT("tovin(255) is ~6.0V (full scale)", telem_tovin(255), 6.0, 0.01);
}

static void test_telem_tovinv2(void)
{
	CHECK_FLOAT("tovinv2(0) is 0V", telem_tovinv2(0), 0.0, 0.001);
	CHECK_FLOAT("tovinv2(255) is ~24.576V (2.048*12 full scale)", telem_tovinv2(255), 24.576, 0.01);
}

static void test_telem_tovout(void)
{
	CHECK_FLOAT("tovout(0) is 0V", telem_tovout(0), 0.0, 0.001);
	CHECK_FLOAT("tovout(255) is ~1.024V (2.048/2 full scale)", telem_tovout(255), 1.024, 0.01);
}

static void test_telem_totemp(void)
{
	/* ch==0 is the impossible-in-practice sentinel for "no reading yet",
	 * mapped to TELEM_INVTEMP rather than the otherwise-linear -50C. */
	CHECK("totemp(0) is the invalid-temp sentinel", telem_totemp(0), TELEM_INVTEMP);
	/* domain is 0..218; anything above clamps to the known max rather
	 * than continuing the linear formula out of range. */
	CHECK_FLOAT("totemp(218) is 125.0C (top of linear range)", telem_totemp(218), 125.0, 0.01);
	CHECK_FLOAT("totemp(219) clamps to 125.0C", telem_totemp(219), 125.0, 0.001);
	CHECK_FLOAT("totemp(255) clamps to 125.0C", telem_totemp(255), 125.0, 0.001);
}

static void test_corev_totelem(void)
{
	CHECK("corev_totelem(0) is 0", corev_totelem(0), 0);
	CHECK("corev_totelem(500) is 100 (linear /5)", corev_totelem(500), 100);
	CHECK("corev_totelem(-10) clamps to 0", corev_totelem(-10), 0);
	CHECK("corev_totelem(600) clamps to 100", corev_totelem(600), 100);
}

static void test_telem_toiinout(void)
{
	CHECK_FLOAT("toiinout(0, hi) is 0A", telem_toiinout(0, true), 0.0, 0.001);
	CHECK_FLOAT("toiinout(255, hi) is ~40.96A (2.048*20 full scale)", telem_toiinout(255, true), 40.96, 0.01);
	CHECK_FLOAT("toiinout(255, lo) is ~6.4A (2.048*3.125 full scale)", telem_toiinout(255, false), 6.4, 0.01);
}

static void test_telem_totach(void)
{
	/* linear rpm = ch*30, no rounding involved (ch is an exact integer
	 * 0..255 and 30.0 keeps every product exactly representable), so
	 * these can be checked exactly rather than with a tolerance. */
	CHECK("totach(0) is 0rpm", telem_totach(0), 0);
	CHECK("totach(255) is 7650rpm (the byte-encoding ceiling)", telem_totach(255), 7650);

	/* Regression fixture: bytes reverse-engineered from a real Compac A2
	 * (telem_version 0x20) setfan ramp captured during hardware testing -
	 * stats' Fan= readings of 690/4620rpm at setfan 10%/50% correspond to
	 * raw tach bytes 23/154 (Fan / 30). */
	CHECK("totach(23) matches live-hardware setfan~10% reading", telem_totach(23), 690);
	CHECK("totach(154) matches live-hardware setfan~50% reading", telem_totach(154), 4620);
}

static void test_telem_tach_saturated(void)
{
	/* Hardware-observed: setfan 90% saturates the byte at 255 -> exactly
	 * 7650rpm; that's the only value this can reliably flag (see the
	 * doc comment on telem_tach_saturated() - lower wrapped bytes, like
	 * the 270/630rpm seen at setfan 95%/100% on the same hardware, are
	 * indistinguishable from genuine low readings). */
	CHECK("just under the ceiling is not saturated", telem_tach_saturated(7649.9f), false);
	CHECK("exactly at the ceiling is saturated", telem_tach_saturated(7650.0f), true);
	CHECK("totach(255) itself reports saturated", telem_tach_saturated(telem_totach(255)), true);
	CHECK("totach(254) (just under ceiling) is not saturated", telem_tach_saturated(telem_totach(254)), false);
	CHECK("a low, wrapped-looking reading is NOT flagged (known limitation)",
	      telem_tach_saturated(telem_totach(9)), false);
}

int main(void)
{
	test_telem_tovin();
	test_telem_tovinv2();
	test_telem_tovout();
	test_telem_totemp();
	test_corev_totelem();
	test_telem_toiinout();
	test_telem_totach();
	test_telem_tach_saturated();

	TEST_EXIT();
}
