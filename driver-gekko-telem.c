/*
 * Copyright 2025-2026 MacEntyre
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; version 3 of the License.
 * See COPYING for more details.
 */

#include "driver-gekko-telem.h"
#include "miner.h"

float telem_tovin(unsigned char ch)
{
	// value 0..255
	// linear volt 0..6.0
	return (float)(ch) * (6.0 / 255.0);
}

float telem_tovinv2(unsigned char ch)
{
	// value 0..255
	// linear volt 0..(2.048*12)
	return (float)(ch) * (2.048 / 255.0) * 12.0;
}

float telem_tovout(unsigned char ch)
{
	float vout;

	// value 0..255
	// linear volt always 0..2.048 across 2 chips
	vout = (float)(ch) * (2.048 / 255.0) / 2.0;
	return vout;
}

float telem_totemp(unsigned char ch)
{
	// rather than the impossible -50
	if (ch == 0)
		return TELEM_INVTEMP;

	if (ch > 218)
		return 125.0;

	// value 0..218
	// linear temp -50..125 = 175 range
	return (float)(ch) * (175.0 / 218.0) - 50.0;
}

unsigned char corev_totelem(int corev)
{
	int telem;

	// value 0..500
	if (corev < 0)
		corev = 0;
	if (corev > 500)
		corev = 500;

	// linear telem 0..100
	telem = corev / 5;

	return (unsigned char)(telem);
}

// same calc for both iin and iout
float telem_toiinout(unsigned char ch, bool hi)
{
	// value 0..255
	// hi linear current 0..(2.048*20) (40.96)
	// lo linear current 0..(2.048*3.125) (6.4)
	float factor;
	if (hi)
		factor = 20.0;
	else
		factor = 3.125;

	return (float)(ch) * (2.048 / 255.0) * factor;
}

float telem_totach(unsigned char ch)
{
	applog(LOG_DEBUG, "%s(%d)->%d", __func__, (int)ch, (int)ch * 30);
	// value 0..255
	// linear rpm ch*30
	return (float)(ch) * 30.0;
}
