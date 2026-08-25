/****************************************************************************
 * apps/system/c6probe/esp_hosted.h
 *
 * Host-side ESP-Hosted SDIO transport for the ESP32-C6 companion radio on
 * the ESP32-P4 Function EV Board.  Protocol constants and framing follow
 * the official esp_hosted 2.12.12 component.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_SYSTEM_C6PROBE_ESP_HOSTED_H
#define __APPS_SYSTEM_C6PROBE_ESP_HOSTED_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/nuttx.h>

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Interface types multiplexed over the single SDIO link.  Mirrors
 * esp_hosted_interface.h.
 */

#define ESP_HOSTED_IF_INVALID   0
#define ESP_HOSTED_IF_STA       1
#define ESP_HOSTED_IF_AP        2
#define ESP_HOSTED_IF_SERIAL    3
#define ESP_HOSTED_IF_HCI       4
#define ESP_HOSTED_IF_PRIV      5
#define ESP_HOSTED_IF_TEST      6
#define ESP_HOSTED_IF_ETH       7
#define ESP_HOSTED_IF_MAX       8

/* Private interface packet and event types (esp_hosted_transport.h) */

#define ESP_HOSTED_PACKET_TYPE_EVENT 0x33
#define ESP_HOSTED_PRIV_EVENT_INIT   0x22

/* INIT event TLV tags (esp_hosted_transport_init.h) */

#define ESP_HOSTED_PRIV_CAPABILITY       0x11
#define ESP_HOSTED_PRIV_FIRMWARE_CHIP_ID 0x12
#define ESP_HOSTED_PRIV_TEST_RAW_TP      0x13
#define ESP_HOSTED_PRIV_RX_Q_SIZE        0x14
#define ESP_HOSTED_PRIV_TX_Q_SIZE        0x15
#define ESP_HOSTED_PRIV_CAP_EXT          0x16
#define ESP_HOSTED_PRIV_FIRMWARE_VERSION 0x17

/* Tags the host sends back in its own INIT event (SLAVE_CONFIG_PRIV_TAG_TYPE
 * in esp_hosted_transport.h).  The slave keeps the control path silent until
 * it receives this reply.
 */

#define ESP_HOSTED_HOST_CAPABILITIES        0x44
#define ESP_HOSTED_RCVD_CHIP_ID             0x45
#define ESP_HOSTED_SLV_CONFIG_TEST_RAW_TP   0x46
#define ESP_HOSTED_SLV_CONFIG_THROTTLE_HIGH 0x47
#define ESP_HOSTED_SLV_CONFIG_THROTTLE_LOW  0x48

/* Capability bits reported by the INIT event */

#define ESP_HOSTED_CAP_WLAN_SDIO    (1 << 0)
#define ESP_HOSTED_CAP_BT_UART      (1 << 1)
#define ESP_HOSTED_CAP_BT_SDIO      (1 << 2)
#define ESP_HOSTED_CAP_BLE_ONLY     (1 << 3)
#define ESP_HOSTED_CAP_BR_EDR_ONLY  (1 << 4)
#define ESP_HOSTED_CAP_CHECKSUM     (1 << 7)

/* Framing limits (sdio_reg.h) */

#define ESP_HOSTED_HEADER_SIZE      12
#define ESP_HOSTED_BLOCK_SIZE       512
#define ESP_HOSTED_RX_BUFFER_SIZE   1536
#define ESP_HOSTED_MAX_FRAME        2048

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* 12-byte frame header shared by every interface.  Layout must match
 * struct esp_payload_header exactly.  The ESP32-P4 is little endian, so the
 * 16-bit fields are used directly.
 */

begin_packed_struct struct esp_hosted_header_s
{
  uint8_t  if_type : 4;
  uint8_t  if_num  : 4;
  uint8_t  flags;
  uint16_t len;
  uint16_t offset;
  uint16_t checksum;
  uint16_t seq_num;
  uint8_t  throttle_cmd : 2;
  uint8_t  reserved     : 6;

  /* Doubles as hci_pkt_type on the HCI interface and priv_pkt_type on the
   * private interface.  Must stay last.
   */

  uint8_t  pkt_type;
} end_packed_struct;

/* Capabilities announced by the C6 in its INIT event */

struct esp_hosted_caps_s
{
  uint8_t  chip_id;
  uint8_t  capability;
  uint8_t  raw_tp;
  uint8_t  rx_q_size;
  uint8_t  tx_q_size;
  bool     valid;
};

/* Receive callback.  Invoked from the caller's polling context, never from
 * interrupt context.  payload excludes the 12-byte header.
 */

typedef CODE void (*esp_hosted_rx_cb_t)(FAR void *arg, uint8_t if_num,
                                        FAR const uint8_t *payload,
                                        uint16_t len, uint8_t pkt_type);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: esp_hosted_initialize
 *
 * Description:
 *   Reset the C6, enumerate its SDIO Function 1, switch to 4-bit/20MHz and
 *   open the ESP-Hosted data path.  Idempotent: returns OK immediately when
 *   the link is already up, so the C6 is not reset again.
 *
 * Input Parameters:
 *   verbose - Print each protocol step, as the c6probe diagnostic does
 *
 * Returned Value:
 *   OK on success; a negated errno on failure.
 *
 ****************************************************************************/

int esp_hosted_initialize(bool verbose);

/****************************************************************************
 * Name: esp_hosted_register
 *
 * Description:
 *   Attach a receive callback to one interface type.
 *
 ****************************************************************************/

int esp_hosted_register(uint8_t if_type, esp_hosted_rx_cb_t cb,
                        FAR void *arg);

/****************************************************************************
 * Name: esp_hosted_poll
 *
 * Description:
 *   Read at most one pending frame from the C6 and dispatch it to the
 *   registered callback.
 *
 * Returned Value:
 *   1 if a frame was dispatched, 0 if nothing was pending, or a negated
 *   errno on failure.
 *
 ****************************************************************************/

int esp_hosted_poll(void);

/****************************************************************************
 * Name: esp_hosted_send
 *
 * Description:
 *   Send one frame to the C6.  On the HCI interface the caller passes a
 *   complete H4 packet: its leading type byte is moved into the header, as
 *   the official host does.
 *
 * Returned Value:
 *   OK on success; -EAGAIN when the C6 has no free receive buffer, or
 *   another negated errno on failure.
 *
 ****************************************************************************/

int esp_hosted_send(uint8_t if_type, uint8_t if_num,
                    FAR const uint8_t *payload, uint16_t len);

/****************************************************************************
 * Name: esp_hosted_debug_counters
 *
 * Description:
 *   Read the slave flow-control accumulators.  tokens is the raw
 *   TOKEN_RDATA register, pending the number of unread RX bytes the slave
 *   reports, and intr the raw interrupt status.  Used to tell whether the C6
 *   actually consumed a transmitted frame.
 *
 ****************************************************************************/

int esp_hosted_debug_counters(FAR uint32_t *tokens, FAR uint32_t *pending,
                              FAR uint32_t *intr);

/****************************************************************************
 * Name: esp_hosted_probe_alive
 *
 * Description:
 *   Read CCCR and IORDY with CMD52 on Function 0.  Succeeds whenever the
 *   card's SDIO core still responds, so it distinguishes a crashed C6 from
 *   one that is alive but has stopped answering ESP-Hosted requests.
 *
 ****************************************************************************/

int esp_hosted_probe_alive(FAR uint8_t *cccr, FAR uint8_t *iordy);

/****************************************************************************
 * Name: esp_hosted_get_caps
 *
 * Description:
 *   Return the capabilities decoded from the C6 INIT event.
 *
 ****************************************************************************/

FAR const struct esp_hosted_caps_s *esp_hosted_get_caps(void);

/****************************************************************************
 * Name: esp_hosted_rpc_get_mac
 *
 * Description:
 *   Send the GetMacAddress control RPC and wait for the C6 reply.
 *   wifi_if: 0 = STA, 1 = AP.
 *
 ****************************************************************************/

int esp_hosted_rpc_get_mac(int wifi_if, uint8_t mac[6]);

/****************************************************************************
 * Name: esp_hosted_rpc_wifi_init / set_mode / start
 *
 * Description:
 *   Run the Wi-Fi bring-up sequence the reference host performs: esp_wifi_init
 *   with WIFI_INIT_CONFIG_DEFAULT() values, set mode, then start.  Each is a
 *   synchronous control RPC.
 *
 ****************************************************************************/

int esp_hosted_rpc_wifi_init(void);
int esp_hosted_rpc_wifi_set_mode(int mode);
int esp_hosted_rpc_wifi_start(void);

/****************************************************************************
 * Name: esp_hosted_rpc_wifi_scan
 *
 * Description:
 *   Run a blocking scan and print the APs the C6 reports.
 *
 ****************************************************************************/

int esp_hosted_rpc_wifi_scan(void);

/****************************************************************************
 * Name: esp_hosted_rpc_wifi_connect
 *
 * Description:
 *   Program STA credentials and associate.  Credentials are supplied by the
 *   caller so nothing sensitive is compiled in.
 *
 ****************************************************************************/

int esp_hosted_rpc_wifi_connect(FAR const char *ssid, FAR const char *pwd);

#endif /* __APPS_SYSTEM_C6PROBE_ESP_HOSTED_H */
