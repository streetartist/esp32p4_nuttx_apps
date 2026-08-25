/****************************************************************************
 * apps/system/c6probe/c6net.h
 ****************************************************************************/

#ifndef __APPS_SYSTEM_C6PROBE_C6NET_H
#define __APPS_SYSTEM_C6PROBE_C6NET_H

#include <nuttx/config.h>
#include <errno.h>
#include <nuttx/compiler.h>

#ifdef CONFIG_NET
int c6net_initialize(FAR const char *ssid, FAR const char *password);
#else
static inline int c6net_initialize(FAR const char *ssid,
  FAR const char *password)
{
  (void)ssid;
  (void)password;
  return -ENOSYS;
}
#endif

#endif
