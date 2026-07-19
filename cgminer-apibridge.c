/*
 * Copyright 2026 MacEntyre
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

/* Fork/exec + supervision of the apibridge companion process. This file is
 * the only integration point between cgminer.c and apibridge/ - it must
 * never be able to take mining down: any failure here is logged and simply
 * results in no apibridge running, mining continues unaffected. */

#include "config.h"

#ifdef USE_APIBRIDGE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "miner.h"
#include "cgminer-apibridge.h"

#define APIBRIDGE_READY_TIMEOUT_MS 10000
#define APIBRIDGE_TOKEN_BYTES 32
#define APIBRIDGE_BACKOFF_MIN_MS 1000
#define APIBRIDGE_BACKOFF_MAX_MS 30000
#define APIBRIDGE_HEALTHY_UPTIME_S 60
#define APIBRIDGE_STOP_TIMEOUT_MS 2000

static pid_t apibridge_pid;
static volatile bool apibridge_shutting_down;
static volatile bool apibridge_supervised;
static cgsem_t apibridge_stopped_sem;
static char apibridge_token[APIBRIDGE_TOKEN_BYTES * 2 + 1];
static char apibridge_write_token[APIBRIDGE_TOKEN_BYTES * 2 + 1];

/* token_out must point at a buffer of at least APIBRIDGE_TOKEN_BYTES*2+1
 * bytes. Shared by both the read-only and write-scoped tokens. */
static bool generate_token(char *token_out)
{
	unsigned char raw[APIBRIDGE_TOKEN_BYTES];
	FILE *rnd;
	int i;

	rnd = fopen("/dev/urandom", "rb");
	if (!rnd) {
		applog(LOG_ERR, "apibridge: failed to open /dev/urandom (%s)", strerror(errno));
		return false;
	}
	if (fread(raw, 1, sizeof(raw), rnd) != sizeof(raw)) {
		applog(LOG_ERR, "apibridge: failed to read /dev/urandom");
		fclose(rnd);
		return false;
	}
	fclose(rnd);

	for (i = 0; i < APIBRIDGE_TOKEN_BYTES; i++)
		snprintf(&token_out[i * 2], 3, "%02x", raw[i]);

	return true;
}

/* configured_path overrides <cgminer_path>/default_name when non-NULL.
 * label ("auth"/"write") is only used for log messages. */
static bool write_token_file(char *path_out, size_t path_out_siz, const char *configured_path,
			      const char *default_name, const char *token, const char *label)
{
	int fd;
	FILE *f;

	if (configured_path)
		snprintf(path_out, path_out_siz, "%s", configured_path);
	else
		snprintf(path_out, path_out_siz, "%s%s", cgminer_path, default_name);

	/* Create with restrictive permissions from the outset rather than
	 * chmod'ing afterwards, to avoid a window where the token is
	 * world-readable. */
	fd = open(path_out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		applog(LOG_ERR, "apibridge: failed to open %s token file %s (%s)", label, path_out, strerror(errno));
		return false;
	}

	f = fdopen(fd, "w");
	if (!f) {
		applog(LOG_ERR, "apibridge: fdopen failed for %s token file %s (%s)", label, path_out, strerror(errno));
		close(fd);
		return false;
	}
	fprintf(f, "%s\n", token);
	fclose(f);

	applog(LOG_WARNING, "apibridge: %s token written to %s", label, path_out);
	return true;
}

/* Defense in depth: the API listening socket is the fd we know for certain
 * gets inherited across fork()/exec() without this (see api.c's FD_CLOEXEC
 * fix), but rather than track down every other fd cgminer might have open
 * at this point (USB handles, log files, ...), just close everything above
 * stderr before exec'ing the child. */
static void close_inherited_fds(void)
{
	int fd, maxfd = (int)sysconf(_SC_OPEN_MAX);

	if (maxfd < 0)
		maxfd = 1024;
	for (fd = 3; fd < maxfd; fd++)
		close(fd);
}

/* Child process only - exec the apibridge binary, located alongside cgminer
 * itself. Never returns. */
static void exec_apibridge_child(void)
{
	char apibridge_path[PATH_MAX];
	char port_s[8], bridge_port_s[8];
	char *argv_[8];
	int argc_ = 0;

	close_inherited_fds();

	snprintf(apibridge_path, sizeof(apibridge_path), "%sapibridged", cgminer_path);
	snprintf(port_s, sizeof(port_s), "%d", opt_api_port);
	snprintf(bridge_port_s, sizeof(bridge_port_s), "%d", opt_api_bridge_port);

	argv_[argc_++] = apibridge_path;
	argv_[argc_++] = "--cgminer-host";
	argv_[argc_++] = "127.0.0.1";
	argv_[argc_++] = "--cgminer-port";
	argv_[argc_++] = port_s;
	argv_[argc_++] = "--listen-port";
	argv_[argc_++] = bridge_port_s;
	argv_[argc_++] = NULL;

	/* Tokens go via env var, not argv, so they don't show up in `ps`. The
	 * write token is only set when control endpoints are enabled - its
	 * mere presence in apibridge's environment is what apibridge itself
	 * uses to decide whether to register the control routes at all. */
	setenv("CGMINER_APIBRIDGE_TOKEN", apibridge_token, 1);
	if (opt_api_bridge_control)
		setenv("CGMINER_APIBRIDGE_WRITE_TOKEN", apibridge_write_token, 1);
	if (opt_api_bridge_bind)
		setenv("CGMINER_APIBRIDGE_BIND", opt_api_bridge_bind, 1);

	execv(apibridge_path, (EXECV_2ND_ARG_TYPE)argv_);
	/* execv only returns on failure */
	applog(LOG_ERR, "apibridge: execv of %s failed (%s)", apibridge_path, strerror(errno));
	_exit(1);
}

static pid_t spawn_apibridge_child(void)
{
	pid_t pid = fork();

	if (pid < 0) {
		applog(LOG_ERR, "apibridge: fork failed (%s)", strerror(errno));
		return -1;
	}
	if (pid == 0)
		exec_apibridge_child(); /* never returns */

	applog(LOG_NOTICE, "apibridge: started, pid %d", (int)pid);
	return pid;
}

static void *apibridge_babysitter_thread(void __maybe_unused *arg)
{
	int backoff_ms = APIBRIDGE_BACKOFF_MIN_MS;
	time_t last_spawn = time(NULL);

	pthread_detach(pthread_self());

	while (1) {
		int status;
		pid_t waited = waitpid(apibridge_pid, &status, 0);

		if (waited < 0) {
			if (errno == EINTR)
				continue;
			applog(LOG_ERR, "apibridge: waitpid failed (%s), giving up supervision", strerror(errno));
			apibridge_pid = 0;
			break;
		}

		if (apibridge_shutting_down) {
			apibridge_pid = 0;
			cgsem_post(&apibridge_stopped_sem);
			break;
		}

		applog(LOG_WARNING, "apibridge: process (pid %d) exited unexpectedly, respawning in %dms",
		       (int)waited, backoff_ms);

		if (time(NULL) - last_spawn > APIBRIDGE_HEALTHY_UPTIME_S)
			backoff_ms = APIBRIDGE_BACKOFF_MIN_MS;

		cgsleep_ms(backoff_ms);
		backoff_ms = (backoff_ms * 2 > APIBRIDGE_BACKOFF_MAX_MS) ? APIBRIDGE_BACKOFF_MAX_MS : backoff_ms * 2;

		last_spawn = time(NULL);
		apibridge_pid = spawn_apibridge_child();
		if (apibridge_pid < 0) {
			apibridge_pid = 0;
			break;
		}
	}
	return NULL;
}

void start_apibridge(void)
{
	char token_path[PATH_MAX];
	pthread_t babysitter;

	if (!opt_api_bridge)
		return;

	if (!opt_api_listen) {
		applog(LOG_WARNING, "apibridge: --api-bridge requires the local API, auto-enabling --api-listen");
		opt_api_listen = true;
	}

	if (cgsem_mswait(&api_ready_sem, APIBRIDGE_READY_TIMEOUT_MS) || !cgminer_api_listening) {
		applog(LOG_ERR, "apibridge: local API did not come up in time, not starting apibridge");
		return;
	}

	if (!generate_token(apibridge_token))
		return;
	if (!write_token_file(token_path, sizeof(token_path), opt_api_bridge_token_file,
			       "apibridge.token", apibridge_token, "auth"))
		return;

	if (opt_api_bridge_control) {
		char write_token_path[PATH_MAX];

		if (!generate_token(apibridge_write_token))
			return;
		if (!write_token_file(write_token_path, sizeof(write_token_path), opt_api_bridge_write_token_file,
				       "apibridge-write.token", apibridge_write_token, "write"))
			return;
		applog(LOG_WARNING, "apibridge: control endpoints enabled - this requires cgminer to "
				     "also be started with --api-allow granting W to 127.0.0.1, e.g. "
				     "--api-allow W:127.0.0.1, or all control requests will be denied");
	}

	cgsem_init(&apibridge_stopped_sem);

	apibridge_pid = spawn_apibridge_child();
	if (apibridge_pid < 0) {
		apibridge_pid = 0;
		return;
	}

	if (pthread_create(&babysitter, NULL, apibridge_babysitter_thread, NULL)) {
		applog(LOG_ERR, "apibridge: failed to start babysitter thread, terminating unsupervised child");
		kill(apibridge_pid, SIGTERM);
		apibridge_pid = 0;
		return;
	}
	apibridge_supervised = true;
}

void stop_apibridge(void)
{
	if (apibridge_pid <= 0)
		return;

	apibridge_shutting_down = true;
	kill(apibridge_pid, SIGTERM);

	if (!apibridge_supervised) {
		/* No babysitter thread is watching this child (pthread_create
		 * failed at startup) - reap it directly as a best effort. */
		int status;
		waitpid(apibridge_pid, &status, 0);
		apibridge_pid = 0;
		return;
	}

	if (cgsem_mswait(&apibridge_stopped_sem, APIBRIDGE_STOP_TIMEOUT_MS)) {
		applog(LOG_WARNING, "apibridge: did not exit cleanly, sending SIGKILL");
		kill(apibridge_pid, SIGKILL);
		/* The babysitter thread is the sole waitpid() caller; wait
		 * for it to reap the now-killed child and post. */
		cgsem_wait(&apibridge_stopped_sem);
	}
}

#endif /* USE_APIBRIDGE */
