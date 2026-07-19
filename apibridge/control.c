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

/* GSA1/GSA2 (Compac A2 / Terminus A2) specific options - corev, setfan,
 * zeromaxt - are relayed for any asc_id without checking device type here:
 * compac_api_set() itself no-ops harmlessly on other ASIC types, so
 * duplicating that check would just be re-validation apibridge shouldn't
 * do. See driver-gekko.c:compac_api_set(). */
static const struct control_option control_whitelist[] = {
	{ "freq",       true  },
	{ "target",     true  },
	{ "corev",      true  },
	{ "setfan",     true  },
	{ "lockfreq",   false },
	{ "unlockfreq", false },
	{ "zeromaxt",   false },
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

int control_status_to_http(json_t *resp, const char **msg_out)
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

	if (!strcmp(sev, "S"))
		return 200;

	/* api.c's MSG_ACCDENY text - the ACL rejection specifically, so a
	 * dashboard can tell "write ACL missing" (config problem) apart from
	 * "bad value" (user input problem). */
	if (json_is_string(msg) && strstr(json_string_value(msg), "Access denied"))
		return 403;

	return 422;
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
