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

float telem_tovin(unsigned char ch);
float telem_tovinv2(unsigned char ch);
float telem_tovout(unsigned char ch);
float telem_totemp(unsigned char ch);
unsigned char corev_totelem(int corev);
float telem_toiinout(unsigned char ch, bool hi);
float telem_totach(unsigned char ch);

#endif /* __DRIVER_GEKKO_TELEM_H__ */
