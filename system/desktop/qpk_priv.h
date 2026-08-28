/****************************************************************************
 * apps/system/desktop/qpk_priv.h
 *
 * Internal helpers shared by qpk_runtime.c and qpk_hap.c.
 ****************************************************************************/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <quickjs.h>

JSContext *qpk_js_context(void);
const char *qpk_basedir(void);
void qpk_show_error(const char *prefix);
int qpk_eval_global(const char *filename, const char *source, size_t len);
int qpk_read_file(const char *path, char **out, size_t *outlen);
void qpk_clear_page(void);
int qpk_call_name(const char *name);
int qpk_call_name_budget(const char *name, unsigned int budget_ms);
void qpk_run_gc(void);
bool qpk_in_js_timer(void);
int qpk_widget_count(void);
