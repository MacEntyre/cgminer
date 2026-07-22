/*
 * Copyright 2025-2026 MacEntyre
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; version 3 of the License.
 * See COPYING for more details.
 */

/* Pure byte<->physical-unit conversions for the GSA1/GSA2 (Compac A1/A2)
 * telemetry MCU protocol. Split out of driver-gekko.c so this math can be
 * unit tested (tests/test_gekko_telem.c) without pulling in the rest of the
 * driver's USB/threading/global-state machinery. */

#ifndef __DRIVER_GEKKO_TELEM_H__
#define __DRIVER_GEKKO_TELEM_H__

#include <stdbool.h>

// not possible value meaning invalid
#define TELEM_INVTEMP -999

// value 0..255, ch=255 -> 7650 is the representable ceiling; true rpm may be
// higher and wrapped (byte = true_count mod 256) - there is no way to tell
// from a single byte, so do NOT attempt to "correct" it, only flag it.
#define TELEM_TACH_MAX_RPM 7650.0

float telem_tovin(unsigned char ch);
float telem_tovinv2(unsigned char ch);
float telem_tovout(unsigned char ch);
float telem_totemp(unsigned char ch);
unsigned char corev_totelem(int corev);
float telem_toiinout(unsigned char ch, bool hi);
float telem_totach(unsigned char ch);

// true if the byte hit the representable ceiling - the underlying pulse
// count may be higher and have wrapped; only ch==255 is unambiguously
// flaggable this way, lower wrapped values (e.g. bytes 9/21 seen on real
// hardware at 95%/100% duty) are indistinguishable from genuine low
// readings and cannot be detected from a single byte.
static inline bool telem_tach_saturated(float rpm)
{
	return rpm >= TELEM_TACH_MAX_RPM;
}

#endif /* __DRIVER_GEKKO_TELEM_H__ */
