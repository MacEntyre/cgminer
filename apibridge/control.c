/*
 * Copyright 2026 MacEntyre
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

/* Phase 2 control relay: whitelists which ascset options apibridge will
 * forward, builds the wire parameter, and relays to cgminer's own ascset/
 * privileged RPC commands (api.c). Deliberately does not re-validate
 * frequency/voltage ranges etc itself - driver-gekko.c's compac_api_set()
 * already does that (limit_freq(), fbound()), and is the sole source of
 * truth, per APIBRIDGE-README's Phase 2 roadmap. */

#include <stdio.h>
#include <string.h>

#include "apibridge.h"
#include "cgclient.h"
#include "control.h"

struct control_option {
	const char *name;
	bool needs_value;
};

/* Device-type-specific options - corev/setfan/zeromaxt (GSA1/GSA2, i.e.
 * Compac A2 / Terminus A2) and chip/usbprop (BM1397, i.e. CompacF/R909) -
 * are relayed for any asc_id without checking device type here:
 * compac_api_set() itself no-ops harmlessly (or returns its own "only for
 * ..." error) on other ASIC types, so duplicating that check would just be
 * re-validation apibridge shouldn't do. See driver-gekko.c:compac_api_set(). */
static const struct control_option control_whitelist[] = {
	{ "freq",       true  },
	{ "target",     true  },
	{ "corev",      true  },
	{ "setfan",     true  },
	{ "lockfreq",   false },
	{ "unlockfreq", false },
	{ "zeromaxt",   false },
	{ "chip",       true  },
	{ "waitfactor", true  },
	{ "usbprop",    true  },
	{ "require",    true  },
};

#define CONTROL_WHITELIST_LEN (sizeof(control_whitelist) / sizeof(control_whitelist[0]))

bool control_option_allowed(const char *option, bool *needs_value)
{
	size_t i;

	if (!option || !*option)
		return false;

	for (i = 0; i < CONTROL_WHITELIST_LEN; i++) {
		if (!strcasecmp(option, control_whitelist[i].name)) {
			if (needs_value)
				*needs_value = control_whitelist[i].needs_value;
			return true;
		}
	}

	return false;
}

bool control_build_parameter(char *out, size_t out_siz, int asc_id, const char *option,
			      bool value_present, double value)
{
	int n;

	if (!out || asc_id < 0 || !option || !*option)
		return false;

	if (value_present)
		n = snprintf(out, out_siz, "%d,%s,%g", asc_id, option, value);
	else
		n = snprintf(out, out_siz, "%d,%s", asc_id, option);

	return n > 0 && (size_t)n < out_siz;
}

json_t *control_relay_ascset(int asc_id, const char *option, bool value_present, double value)
{
	char param[CONTROL_PARAM_MAX];

	if (!control_build_parameter(param, sizeof(param), asc_id, option, value_present, value))
		return NULL;

	return cgclient_query_param(g_config.cgminer_host, g_config.cgminer_port, "ascset", param, 3);
}

json_t *control_relay_reset(int asc_id)
{
	char param[CONTROL_PARAM_MAX];

	if (!control_build_parameter(param, sizeof(param), asc_id, "reset", false, 0))
		return NULL;

	return cgclient_query_param(g_config.cgminer_host, g_config.cgminer_port, "ascset", param, 3);
}

bool control_build_parameter_chip(char *out, size_t out_siz, int asc_id, int chip_index, double value)
{
	int n;

	if (!out || asc_id < 0 || chip_index < 0)
		return false;

	n = snprintf(out, out_siz, "%d,chip,%d:%g", asc_id, chip_index, value);

	return n > 0 && (size_t)n < out_siz;
}

json_t *control_relay_ascset_chip(int asc_id, int chip_index, double value)
{
	char param[CONTROL_PARAM_MAX];

	if (!control_build_parameter_chip(param, sizeof(param), asc_id, chip_index, value))
		return NULL;

	return cgclient_query_param(g_config.cgminer_host, g_config.cgminer_port, "ascset", param, 3);
}

/* ascenable/ascdisable take a bare "<asc_id>" wire parameter, not the
 * "<asc_id>,<option>[,<value>]" shape ascset uses (api.c: ascenable(),
 * ascdisable() both just atoi() the whole parameter) - so this builds its
 * own tiny parameter rather than going through control_build_parameter(). */
static json_t *control_relay_asc_command(const char *command, int asc_id)
{
	char param[16];
	int n;

	if (asc_id < 0)
		return NULL;

	n = snprintf(param, sizeof(param), "%d", asc_id);
	if (n <= 0 || (size_t)n >= sizeof(param))
		return NULL;

	return cgclient_query_param(g_config.cgminer_host, g_config.cgminer_port, command, param, 3);
}

json_t *control_relay_ascenable(int asc_id)
{
	return control_relay_asc_command("ascenable", asc_id);
}

json_t *control_relay_ascdisable(int asc_id)
{
	return control_relay_asc_command("ascdisable", asc_id);
}

static int status_to_http(json_t *resp, const char **msg_out, bool info_is_success)
{
	json_t *status_arr, *first, *status_str, *msg;
	const char *sev;

	if (msg_out)
		*msg_out = NULL;

	if (!resp || !json_is_object(resp))
		return 502;

	status_arr = json_object_get(resp, "STATUS");
	if (!json_is_array(status_arr) || json_array_size(status_arr) == 0)
		return 502;

	first = json_array_get(status_arr, 0);
	status_str = json_object_get(first, "STATUS");
	msg = json_object_get(first, "Msg");

	if (msg_out && json_is_string(msg))
		*msg_out = json_string_value(msg);

	sev = json_is_string(status_str) ? json_string_value(status_str) : NULL;
	if (!sev)
		return 502;

	if (!strcmp(sev, "S") || (info_is_success && !strcmp(sev, "I")))
		return 200;

	/* api.c's MSG_ACCDENY text - the ACL rejection specifically, so a
	 * dashboard can tell "write ACL missing" (config problem) apart from
	 * "bad value" (user input problem). */
	if (json_is_string(msg) && strstr(json_string_value(msg), "Access denied"))
		return 403;

	return 422;
}

int control_status_to_http(json_t *resp, const char **msg_out)
{
	return status_to_http(resp, msg_out, false);
}

int control_enable_status_to_http(json_t *resp, const char **msg_out)
{
	return status_to_http(resp, msg_out, true);
}

bool control_check_privileged(void)
{
	json_t *resp;
	int code;

	resp = cgclient_query(g_config.cgminer_host, g_config.cgminer_port, "privileged", 3);
	if (!resp)
		return false;

	code = control_status_to_http(resp, NULL);
	json_decref(resp);

	return code == 200;
}

static bool control_available;

void control_set_available(bool available)
{
	control_available = available;
}

bool control_is_available(void)
{
	return control_available;
}
