#ifndef __APIBRIDGE_HTTPAPI_H__
#define __APIBRIDGE_HTTPAPI_H__

#include <civetweb.h>

/* Registers the Phase 1 read-only REST routes (/api/v1/summary, /devs,
 * /pools, /health) on ctx. */
void httpapi_register(struct mg_context *ctx);

#endif /* __APIBRIDGE_HTTPAPI_H__ */
