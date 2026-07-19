/*
 * Copyright 2026 MacEntyre
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include <string.h>

#include "apibridge.h"
#include "auth.h"

bool auth_check_token_string(const char *presented)
{
	size_t token_len, presented_len, i;
	unsigned char diff = 0;

	if (!presented || !g_config.token)
		return false;

	token_len = strlen(g_config.token);
	presented_len = strlen(presented);

	if (token_len != presented_len)
		return false;

	for (i = 0; i < token_len; i++)
		diff |= (unsigned char)(presented[i] ^ g_config.token[i]);

	return diff == 0;
}

bool auth_check_request(const struct mg_connection *conn)
{
	static const char prefix[] = "Bearer ";
	const char *header = mg_get_header(conn, "Authorization");
	const struct mg_request_info *ri;
	char token_buf[128];

	if (header && strncmp(header, prefix, sizeof(prefix) - 1) == 0)
		return auth_check_token_string(header + sizeof(prefix) - 1);

	ri = mg_get_request_info(conn);
	if (ri && ri->query_string &&
	    mg_get_var(ri->query_string, strlen(ri->query_string), "token", token_buf, sizeof(token_buf)) > 0)
		return auth_check_token_string(token_buf);

	return false;
}
