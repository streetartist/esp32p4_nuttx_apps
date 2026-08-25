/****************************************************************************
 * apps/system/c6probe/c6net.c
 *
 * ESP-Hosted STA Ethernet adapter for the NuttX network stack.
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_NET

#include <errno.h>
#include <net/if.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/net/net.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/netconfig.h>

#include "c6net.h"
#include "esp_hosted.h"

#ifndef CONFIG_NET_ETH_PKTSIZE
#  define CONFIG_NET_ETH_PKTSIZE 1500
#endif

#define C6NET_BUFSIZE (CONFIG_NET_ETH_PKTSIZE + 64)
#define C6NET_POLL_US       10000
#define C6NET_TASK_PRIORITY 100
#define C6NET_TASK_STACK    4096

struct c6net_state_s
{
  struct net_driver_s dev;
  pid_t daemon_pid;
  bool initialized;
  bool ifup;
  bool associated;
  bool event_pending;
  uint8_t buf[C6NET_BUFSIZE] __attribute__((aligned(4)));
};

static struct c6net_state_s g_c6net;

static int c6net_daemon(int argc, FAR char *argv[])
{
  FAR struct c6net_state_s *priv = &g_c6net;

  UNUSED(argc);
  UNUSED(argv);

  while (priv->initialized)
    {
      if (priv->event_pending)
        {
          priv->event_pending = false;
          if (priv->associated && priv->ifup)
            {
              netdev_carrier_on(&priv->dev);
            }
          else
            {
              netdev_carrier_off(&priv->dev);
            }
        }

      while (esp_hosted_poll() > 0)
        {
        }

      usleep(C6NET_POLL_US);
    }

  priv->daemon_pid = -1;
  return 0;
}

static int c6net_txpoll(FAR struct net_driver_s *dev)
{
  if (dev->d_len > 0)
    {
      if (esp_hosted_send(ESP_HOSTED_IF_STA, 0, dev->d_buf,
                          dev->d_len) < 0)
        {
          return -EIO;
        }

      dev->d_len = 0;
    }

  return 0;
}

static int c6net_ifup(FAR struct net_driver_s *dev)
{
  dev->d_flags |= IFF_UP | IFF_RUNNING;
  if (g_c6net.associated)
    {
      netdev_carrier_on(dev);
    }
  return 0;
}

static int c6net_ifdown(FAR struct net_driver_s *dev)
{
  dev->d_flags &= ~(IFF_UP | IFF_RUNNING);
  netdev_carrier_off(dev);
  return 0;
}

static int c6net_txavail(FAR struct net_driver_s *dev)
{
  if ((dev->d_flags & IFF_UP) != 0)
    {
      netdev_lock(dev);
      devif_poll(dev, c6net_txpoll);
      netdev_unlock(dev);
    }

  return 0;
}

static void c6net_wifi_event(FAR void *arg, bool connected)
{
  FAR struct c6net_state_s *priv = arg;

  priv->associated = connected;
  priv->event_pending = true;
}

static void c6net_rx(FAR void *arg, uint8_t if_num,
                     FAR const uint8_t *payload, uint16_t len,
                     uint8_t pkt_type)
{
  FAR struct c6net_state_s *priv = arg;
  uint16_t ethertype;

  UNUSED(if_num);
  UNUSED(pkt_type);

  if (!priv->ifup || len < 14 || len > sizeof(priv->buf))
    {
      return;
    }

  netdev_lock(&priv->dev);
  memcpy(priv->buf, payload, len);
  priv->dev.d_buf = priv->buf;
  priv->dev.d_len = len;

  ethertype = ((uint16_t)priv->buf[12] << 8) | priv->buf[13];
  if (ethertype == ETHTYPE_IP)
    {
      ipv4_input(&priv->dev);
    }
#ifdef CONFIG_NET_IPv6
  else if (ethertype == ETHTYPE_IPV6)
    {
      ipv6_input(&priv->dev);
    }
#endif
#ifdef CONFIG_NET_ARP
  else if (ethertype == ETHTYPE_ARP)
    {
      arp_input(&priv->dev);
    }
#endif

  priv->dev.d_len = 0;
  netdev_unlock(&priv->dev);
}

int c6net_initialize(FAR const char *ssid, FAR const char *password)
{
  FAR struct c6net_state_s *priv = &g_c6net;
  uint8_t mac[6];
  int ret;

  if (priv->initialized)
    {
      return 0;
    }

  ret = esp_hosted_initialize(false);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_rpc_wifi_init();
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_rpc_wifi_set_mode(1);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_rpc_wifi_start();
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_rpc_wifi_connect(ssid, password);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_rpc_get_mac(0, mac);
  if (ret < 0)
    {
      return ret;
    }

  memset(priv, 0, sizeof(*priv));
  memcpy(priv->dev.d_mac.ether.ether_addr_octet, mac, 6);
  priv->dev.d_ifname[0] = 'e';
  priv->dev.d_ifname[1] = 't';
  priv->dev.d_ifname[2] = 'h';
  priv->dev.d_ifname[3] = '0';
  priv->dev.d_llhdrlen = 14;
  priv->dev.d_pktsize = CONFIG_NET_ETH_PKTSIZE;
  priv->dev.d_buf = priv->buf;
  priv->dev.d_ifup = c6net_ifup;
  priv->dev.d_ifdown = c6net_ifdown;
  priv->dev.d_txavail = c6net_txavail;
  priv->dev.d_private = priv;

  ret = esp_hosted_rpc_set_wifi_event_cb(c6net_wifi_event, priv);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_register(ESP_HOSTED_IF_STA, c6net_rx, priv);
  if (ret < 0)
    {
      return ret;
    }

  ret = netdev_register(&priv->dev, NET_LL_ETHERNET);
  if (ret < 0)
    {
      return ret;
    }

  priv->initialized = true;
  priv->ifup = true;
  priv->associated = false;
  c6net_ifup(&priv->dev);

  priv->daemon_pid = task_create("c6net", C6NET_TASK_PRIORITY,
                                 C6NET_TASK_STACK, c6net_daemon, NULL);
  if (priv->daemon_pid < 0)
    {
      priv->initialized = false;
      return -errno;
    }

  return 0;
}

#endif /* CONFIG_NET */
