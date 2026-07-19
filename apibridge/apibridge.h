#ifndef __APIBRIDGE_H__
#define __APIBRIDGE_H__

#include <stdbool.h>

struct apibridge_config {
	char *cgminer_host;
	int cgminer_port;
	int listen_port;
	char *listen_bind;
	char *token;
	/* Write-scoped token gating the control routes (POST /api/v1/control,
	 * /api/v1/control/reset). NULL means control was not enabled by
	 * cgminer (no --api-bridge-control) - the routes then answer 501
	 * rather than checking any token. */
	char *write_token;
	int poll_interval_ms;
};

extern struct apibridge_config g_config;

/* Minimal timestamped stderr logger - apibridge is a standalone process and
 * deliberately has no dependency on cgminer's own logging machinery. */
void bridge_log(const char *fmt, ...);

#endif /* __APIBRIDGE_H__ */
