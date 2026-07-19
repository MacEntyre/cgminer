#ifndef __APIBRIDGE_SETTINGS_H__
#define __APIBRIDGE_SETTINGS_H__

#include "apibridge.h"

/* Parses argv (--cgminer-host, --cgminer-port, --listen-port,
 * --poll-interval-ms) plus CGMINER_APIBRIDGE_TOKEN/CGMINER_APIBRIDGE_BIND/
 * CGMINER_APIBRIDGE_WRITE_TOKEN from the environment into cfg. Prints usage
 * and exit(1)s on bad args or a missing read token - there is no safe
 * default for the auth token. CGMINER_APIBRIDGE_WRITE_TOKEN is optional:
 * its absence just means the control routes stay disabled. */
void config_parse_args(int argc, char **argv, struct apibridge_config *cfg);

#endif /* __APIBRIDGE_SETTINGS_H__ */
