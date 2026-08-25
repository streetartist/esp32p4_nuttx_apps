/****************************************************************************
 * apps/system/c6probe/c6probe_main.c
 *
 * NSH command that brings up the resident ESP-Hosted transport to the
 * ESP32-C6 companion radio and reports the negotiated capabilities.
 * The transport itself lives in esp_hosted.c and stays up after this
 * command returns; running c6probe again is a no-op probe.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_hosted.h"

int main(int argc, FAR char *argv[])
{
  FAR struct esp_hosted_caps_s const *caps;
  uint8_t mac[6];
  int polls;
  int ret;
  int i;

  /* stat reports the low-level link state without touching it, so it can be
   * run straight after a failure to see whether the C6 is still alive.
   */

  if (argc > 1 && strcmp(argv[1], "stat") == 0)
    {
      uint32_t tok = 0;
      uint32_t pend = 0;
      uint32_t intr = 0;
      uint8_t cccr = 0;
      uint8_t iordy = 0;
      int aret;
      int cret;

      aret = esp_hosted_probe_alive(&cccr, &iordy);
      printf("c6probe: CMD52 ret=%d CCCR=0x%02x IORDY=0x%02x\n",
             aret, cccr, iordy);

      cret = esp_hosted_debug_counters(&tok, &pend, &intr);
      printf("c6probe: regs ret=%d TOKEN=%08" PRIx32 " PKTLEN=%08" PRIx32
             " INT=%08" PRIx32 "\n", cret, tok, pend, intr);

      return EXIT_SUCCESS;
    }

  /* connect [<ssid> [<password>]] associates with an AP.  Credentials are
   * given on the command line so they are never compiled into the image.
   * "connect" with no SSID skips SetConfig and associates using whatever
   * configuration the C6 already has stored, which isolates SetConfig.
   */

  if (argc > 1 && strcmp(argv[1], "connect") == 0)
    {
      ret = esp_hosted_initialize(false);
      if (ret < 0)
        {
          fprintf(stderr, "c6probe: bring-up failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      caps = esp_hosted_get_caps();
      for (polls = 0; polls < 20 && !caps->valid; polls++)
        {
          esp_hosted_poll();
          usleep(50000);
        }

      if (esp_hosted_rpc_wifi_init() < 0 ||
      esp_hosted_rpc_wifi_set_mode(1) < 0)
        {
          return EXIT_FAILURE;
        }

      if (esp_hosted_rpc_wifi_start() < 0)
        {
          return EXIT_FAILURE;
        }

      ret = esp_hosted_rpc_wifi_connect(argc > 2 ? argv[2] : "",
                                        argc > 3 ? argv[3] : "");
      if (ret < 0)
        {
          fprintf(stderr, "c6probe: connect failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      /* Association is asynchronous: keep polling so the C6 can push its
       * connect/disconnect events over the control path.
       */

      printf("c6probe: associating, watching events for 10s\n");
      for (polls = 0; polls < 200; polls++)
        {
          esp_hosted_poll();
          usleep(50000);
        }

      return EXIT_SUCCESS;
    }

  /* The wifi subcommand runs the full Wi-Fi bring-up sequence and then
   * reports the STA MAC, mirroring what the reference host does.
   */

  if (argc > 1 && strcmp(argv[1], "wifi") == 0)
    {
      ret = esp_hosted_initialize(false);
      if (ret < 0)
        {
          fprintf(stderr, "c6probe: bring-up failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      caps = esp_hosted_get_caps();
      for (polls = 0; polls < 20 && !caps->valid; polls++)
        {
          esp_hosted_poll();
          usleep(50000);
        }

      if (esp_hosted_rpc_wifi_init() < 0)
        {
          return EXIT_FAILURE;
        }

      if (esp_hosted_rpc_wifi_set_mode(1 /* WIFI_MODE_STA */) < 0)
        {
          return EXIT_FAILURE;
        }

      if (esp_hosted_rpc_wifi_start() < 0)
        {
          return EXIT_FAILURE;
        }

      ret = esp_hosted_rpc_get_mac(0 /* STA */, mac);
      if (ret < 0)
        {
          fprintf(stderr, "c6probe: GetMacAddress RPC failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      printf("c6probe: STA MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

      /* A scan needs no credentials and proves the radio is really up. */

      ret = esp_hosted_rpc_wifi_scan();
      if (ret < 0)
        {
          fprintf(stderr, "c6probe: scan failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      return EXIT_SUCCESS;
    }

  /* The rpc subcommand only exercises the control path. */

  if (argc > 1 && strcmp(argv[1], "rpc") == 0)
    {
      ret = esp_hosted_initialize(false);
      if (ret < 0)
        {
          fprintf(stderr, "c6probe: ESP-Hosted bring-up failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      /* The control path stays silent until the INIT event has been consumed
       * and the host configuration echoed back, so drain it first.
       */

      caps = esp_hosted_get_caps();
      for (polls = 0; polls < 20 && !caps->valid; polls++)
        {
          esp_hosted_poll();
          usleep(50000);
        }

      if (!caps->valid)
        {
          fprintf(stderr, "c6probe: no INIT event; transport not ready\n");
          return EXIT_FAILURE;
        }

      ret = esp_hosted_rpc_get_mac(0 /* STA */, mac);
      if (ret < 0)
        {
          fprintf(stderr, "c6probe: GetMacAddress RPC failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      printf("c6probe: STA MAC ");
      for (i = 0; i < 6; i++)
        {
          printf("%02x%s", mac[i], i < 5 ? ":" : "");
        }

      printf("\n");
      return EXIT_SUCCESS;
    }

  /* Bring up the resident ESP-Hosted transport (idempotent) and consume
   * the INIT event queued at data-path open, which fills in the C6
   * capability record.
   */

  ret = esp_hosted_initialize(true);
  if (ret < 0)
    {
      fprintf(stderr, "c6probe: ESP-Hosted bring-up failed: %d\n", ret);
      return EXIT_FAILURE;
    }

  caps = esp_hosted_get_caps();
  for (polls = 0; polls < 10 && !caps->valid; polls++)
    {
      ret = esp_hosted_poll();
      if (ret < 0)
        {
          fprintf(stderr, "c6probe: transport poll failed: %d\n", ret);
          return EXIT_FAILURE;
        }

      usleep(50000);
    }

  if (!caps->valid)
    {
      fprintf(stderr, "c6probe: no ESP-Hosted INIT event received\n");
      return EXIT_FAILURE;
    }

  printf("c6probe: C6 chip_id=%u capability=%02x rx_q=%u tx_q=%u\n",
         caps->chip_id, caps->capability, caps->rx_q_size, caps->tx_q_size);

  if ((caps->capability & ESP_HOSTED_CAP_WLAN_SDIO) != 0)
    {
      printf("c6probe: C6 supports WLAN over SDIO\n");
    }

  if ((caps->capability & ESP_HOSTED_CAP_BT_SDIO) != 0)
    {
      printf("c6probe: C6 supports BT (HCI) over SDIO\n");
    }

  if ((caps->capability & ESP_HOSTED_CAP_BLE_ONLY) != 0)
    {
      printf("c6probe: C6 is BLE-only (no BR/EDR)\n");
    }

  /* Drain any further pending traffic so the state after c6probe is
   * clean for the resident transport users.
   */

  for (polls = 0; polls < 20; polls++)
    {
      ret = esp_hosted_poll();
      if (ret <= 0)
        {
          break;
        }
    }

  printf("c6probe: OK ESP-Hosted transport is up\n");
  return EXIT_SUCCESS;
}
