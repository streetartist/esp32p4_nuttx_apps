/****************************************************************************
 * apps/system/desktop/qpk_hap.h
 *
 * Xiaomi Quick App / HAP (`$app_define$`) shim for original RPK pages.
 ****************************************************************************/

#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <quickjs.h>

bool qpk_hap_is_source(const char *source, size_t len);
int qpk_hap_install(JSContext *ctx);
int qpk_hap_eval(const char *filename, const char *source, size_t len);
void qpk_hap_reset(void);
