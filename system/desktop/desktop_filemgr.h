/****************************************************************************
 * apps/system/desktop/desktop_filemgr.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#pragma once

#include <stdbool.h>
#include <stddef.h>

int desktop_filemgr_start(void);
bool desktop_filemgr_running(void);
int desktop_filemgr_get_token(char *buffer, size_t buffer_size);
