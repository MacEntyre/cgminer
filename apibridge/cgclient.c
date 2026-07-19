/*
 * Copyright 2026 MacEntyre
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "apibridge.h"
#include "cgclient.h"

#define CGCLIENT_CHUNK 4096
#define CGCLIENT_REQUEST_MAX 256

static json_t *cgclient_send_request(const char *host, int port, const char *request, int req_len,
				      const char *command, int timeout_s)
{
	char port_s[8];
	struct addrinfo hints, *res, *rp;
	int sock = -1;
	struct timeval tv;
	char *buf = NULL;
	size_t buf_len = 0, buf_cap = 0;
	json_t *result;
	json_error_t jerr;

	snprintf(port_s, sizeof(port_s), "%d", port);
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if (getaddrinfo(host, port_s, &hints, &res) != 0) {
		bridge_log("cgclient: getaddrinfo(%s:%d) failed", host, port);
		return NULL;
	}

	for (rp = res; rp; rp = rp->ai_next) {
		sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (sock < 0)
			continue;
		if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0)
			break;
		close(sock);
		sock = -1;
	}
	freeaddrinfo(res);

	if (sock < 0) {
		bridge_log("cgclient: connect to %s:%d failed (%s)", host, port, strerror(errno));
		return NULL;
	}

	tv.tv_sec = timeout_s;
	tv.tv_usec = 0;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	/* Must arrive in a single write(): cgminer's api.c does one recv()
	 * per connection with no partial-read loop. */
	if (write(sock, request, (size_t)req_len) != req_len) {
		bridge_log("cgclient: write failed (%s)", strerror(errno));
		close(sock);
		return NULL;
	}

	while (1) {
		ssize_t n;

		if (buf_len + CGCLIENT_CHUNK > buf_cap) {
			char *grown;

			buf_cap = buf_len + CGCLIENT_CHUNK;
			grown = realloc(buf, buf_cap);
			if (!grown) {
				bridge_log("cgclient: out of memory reading response");
				free(buf);
				close(sock);
				return NULL;
			}
			buf = grown;
		}
		n = read(sock, buf + buf_len, CGCLIENT_CHUNK);
		if (n < 0) {
			bridge_log("cgclient: read failed (%s)", strerror(errno));
			free(buf);
			close(sock);
			return NULL;
		}
		if (n == 0)
			break; /* server closed the connection - full response received */
		buf_len += (size_t)n;
	}
	close(sock);

	if (buf_len == 0) {
		bridge_log("cgclient: empty response for command %s", command);
		free(buf);
		return NULL;
	}

	/* cgminer's api.c reply convention includes a trailing NUL byte after
	 * the JSON object - strip it before parsing, jansson otherwise treats
	 * it as trailing garbage after a complete document. */
	if (buf[buf_len - 1] == '\0')
		buf_len--;

	result = json_loadb(buf, buf_len, 0, &jerr);
	free(buf);
	if (!result) {
		bridge_log("cgclient: failed to parse JSON response for %s: %s", command, jerr.text);
		return NULL;
	}

	return result;
}

json_t *cgclient_query(const char *host, int port, const char *command, int timeout_s)
{
	char request[CGCLIENT_REQUEST_MAX];
	int req_len;

	req_len = snprintf(request, sizeof(request), "{\"command\":\"%s\"}", command);
	if (req_len <= 0 || (size_t)req_len >= sizeof(request)) {
		bridge_log("cgclient: command too long: %s", command);
		return NULL;
	}

	return cgclient_send_request(host, port, request, req_len, command, timeout_s);
}

json_t *cgclient_query_param(const char *host, int port, const char *command, const char *parameter,
			      int timeout_s)
{
	char request[CGCLIENT_REQUEST_MAX];
	int req_len;

	req_len = snprintf(request, sizeof(request), "{\"command\":\"%s\",\"parameter\":\"%s\"}",
			    command, parameter);
	if (req_len <= 0 || (size_t)req_len >= sizeof(request)) {
		bridge_log("cgclient: command+parameter too long: %s %s", command, parameter);
		return NULL;
	}

	return cgclient_send_request(host, port, request, req_len, command, timeout_s);
}
