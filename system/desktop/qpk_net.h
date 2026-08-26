#pragma once

#include "quickjs.h"

/* Start one-time network preparation before the first HTTPS request. */
int qpk_net_prepare(void);

/* Install the network-facing quick-app compatibility API. */
int qpk_net_install(JSContext *ctx);

/* Settle completed requests on the QuickJS owner thread. */
int qpk_net_poll(JSContext *ctx);

/* Detach all requests before destroying their QuickJS context. */
void qpk_net_cancel(JSContext *ctx);
