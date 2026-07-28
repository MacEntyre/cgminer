#ifndef __APIBRIDGE_HTTPAPI_H__
#define __APIBRIDGE_HTTPAPI_H__

#include <civetweb.h>

/* Registers the Phase 1 read-only REST routes (/api/v1/summary, /devs,
 * /pools, /health) plus the Phase 2 control routes (/api/v1/control,
 * /api/v1/control/reset - registered unconditionally, but they answer 501
 * until a write token is configured, see control.c), plus the embedded
 * OpenAPI docs routes (/openapi.yaml, /docs, /docs/redoc.standalone.js -
 * see docs/openapi.yaml and tools/embed_file.sh) on ctx. */
void httpapi_register(struct mg_context *ctx);

#endif /* __APIBRIDGE_HTTPAPI_H__ */
