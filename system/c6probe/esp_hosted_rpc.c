/****************************************************************************
 * apps/system/c6probe/esp_hosted_rpc.c
 *
 * Minimal ESP-Hosted control-path client.  Packs protobuf-c RPC envelopes
 * (from the official esp_hosted_rpc.pb-c.c) and exchanges them over the
 * ESP_SERIAL_IF of the resident transport.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <nuttx/semaphore.h>

#include "esp_hosted.h"
#include "esp_hosted_rpc.pb-c.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* wifi_interface_t from ESP-IDF: STA=0, AP=1 */

#define RPC_WIFI_IF_STA   0
#define RPC_WIFI_IF_AP    1

#define RPC_RESP_WAIT_MS  5000
#define RPC_POLL_INTERVAL_MS 20

/* The control path is not raw protobuf: esp_hosted wraps it in a TLV
 * envelope (host/drivers/virtual_serial_if/serial_if.c, compose_tlv):
 *
 *   | 0x01 | ep_len(LE16) | ep_name | 0x02 | data_len(LE16) | protobuf |
 *
 * Both directions use it; the slave replies on "RPCRsp" and pushes
 * unsolicited events on "RPCEvt".
 */

#define RPC_TLV_T_EPNAME  0x01
#define RPC_TLV_T_DATA    0x02
#define RPC_EP_NAME_RSP   "RPCRsp"
#define RPC_EP_NAME_EVT   "RPCEvt"
#define RPC_TLV_MAX       512

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rpc_client_s
{
  sem_t resp_sem;
  FAR Rpc *resp;          /* Unpacked response, set by the RX callback */
  uint32_t wait_uid;
  bool busy;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rpc_client_s g_rpc =
{
  .resp_sem = SEM_INITIALIZER(0),
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rpc_tlv_compose
 *
 * Description:
 *   Wrap a packed protobuf message in the esp_hosted control-path TLV.
 *
 * Returned Value:
 *   Total TLV length, or a negated errno if it would not fit.
 *
 ****************************************************************************/

static int rpc_tlv_compose(FAR uint8_t *buf, size_t buflen,
                           FAR const uint8_t *data, uint16_t datalen)
{
  size_t eplen = strlen(RPC_EP_NAME_RSP);
  size_t total = 1 + 2 + eplen + 1 + 2 + datalen;
  size_t pos = 0;

  if (total > buflen)
    {
      return -ENOSPC;
    }

  buf[pos++] = RPC_TLV_T_EPNAME;
  buf[pos++] = (uint8_t)(eplen & 0xff);
  buf[pos++] = (uint8_t)((eplen >> 8) & 0xff);
  memcpy(&buf[pos], RPC_EP_NAME_RSP, eplen);
  pos += eplen;

  buf[pos++] = RPC_TLV_T_DATA;
  buf[pos++] = (uint8_t)(datalen & 0xff);
  buf[pos++] = (uint8_t)((datalen >> 8) & 0xff);
  memcpy(&buf[pos], data, datalen);
  pos += datalen;

  return (int)pos;
}

/****************************************************************************
 * Name: rpc_tlv_parse
 *
 * Description:
 *   Strip the control-path TLV and return the protobuf payload.  Accepts
 *   both the response and the event endpoint.
 *
 * Returned Value:
 *   OK on success, with *payload/*paylen set; a negated errno otherwise.
 *
 ****************************************************************************/

static int rpc_tlv_parse(FAR const uint8_t *tlv, uint16_t tlvlen,
                         FAR const uint8_t **payload, FAR uint16_t *paylen)
{
  uint16_t eplen;
  uint16_t datalen;
  uint16_t pos = 0;

  if (tlvlen < 3 || tlv[pos++] != RPC_TLV_T_EPNAME)
    {
      return -EINVAL;
    }

  eplen = (uint16_t)tlv[pos] | ((uint16_t)tlv[pos + 1] << 8);
  pos += 2;

  if (pos + eplen + 3 > tlvlen)
    {
      return -EINVAL;
    }

  if ((eplen != strlen(RPC_EP_NAME_RSP) ||
       memcmp(&tlv[pos], RPC_EP_NAME_RSP, eplen) != 0) &&
      (eplen != strlen(RPC_EP_NAME_EVT) ||
       memcmp(&tlv[pos], RPC_EP_NAME_EVT, eplen) != 0))
    {
      return -EINVAL;
    }

  pos += eplen;

  if (tlv[pos++] != RPC_TLV_T_DATA)
    {
      return -EINVAL;
    }

  datalen = (uint16_t)tlv[pos] | ((uint16_t)tlv[pos + 1] << 8);
  pos += 2;

  if (pos + datalen > tlvlen)
    {
      return -EINVAL;
    }

  *payload = &tlv[pos];
  *paylen  = datalen;
  return OK;
}

/****************************************************************************
 * Name: rpc_rx_cb
 *
 * Description:
 *   Receive callback for the ESP_SERIAL_IF.  Strips the TLV, unpacks the
 *   protobuf envelope and posts the response semaphore when the UID matches
 *   an outstanding request.
 *
 ****************************************************************************/

static void rpc_rx_cb(FAR void *arg, uint8_t if_num,
                      FAR const uint8_t *payload, uint16_t len,
                      uint8_t pkt_type)
{
  FAR struct rpc_client_s *cli = &g_rpc;
  FAR const uint8_t *pb;
  uint16_t pblen;
  FAR Rpc *msg;
  int ret;

  ret = rpc_tlv_parse(payload, len, &pb, &pblen);
  if (ret < 0)
    {
      printf("rpc: bad TLV, len=%u ret=%d\n", len, ret);
      return;
    }

  msg = rpc__unpack(NULL, pblen, pb);
  if (msg == NULL)
    {
      printf("rpc: unpack failed, pblen=%u (frame len=%u)\n", pblen, len);
      return;
    }

  printf("rpc: rx type=%d id=%d uid=%" PRIu32 " case=%d (waiting uid=%"
         PRIu32 ")\n", msg->msg_type, msg->msg_id, msg->uid,
         msg->payload_case, cli->wait_uid);

  if (msg->msg_type == RPC_TYPE__Resp && msg->uid == cli->wait_uid)
    {
      cli->resp = msg;
      sem_post(&cli->resp_sem);
    }
  else
    {
      /* Unsolicited traffic: the slave pushes association state changes here,
       * so surface them instead of dropping silently.
       */

      if (msg->msg_type == RPC_TYPE__Event)
        {
          printf("rpc: EVENT msg_id=%d\n", msg->msg_id);

          if (msg->payload_case == RPC__PAYLOAD_EVENT_STA_CONNECTED)
            {
              printf("rpc: EVENT StaConnected\n");
            }
          else if (msg->payload_case == RPC__PAYLOAD_EVENT_STA_DISCONNECTED)
            {
              printf("rpc: EVENT StaDisconnected\n");
            }
        }

      rpc__free_unpacked(msg, NULL);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rpc_transact
 *
 * Description:
 *   Send a packed Rpc request and wait for the matching response.  Returns
 *   the unpacked response (caller must rpc__free_unpacked it) or NULL.
 *
 ****************************************************************************/

static FAR Rpc *rpc_transact(FAR const Rpc *req, FAR const char *label)
{
  FAR struct rpc_client_s *cli = &g_rpc;
  uint8_t buf[256];
  uint8_t tlv[RPC_TLV_MAX];
  size_t packed;
  int tlvlen;
  int polls;
  int ret;

  packed = rpc__pack(req, buf);
  if (packed > sizeof(buf))
    {
      printf("rpc: %s packed %u exceeds buffer\n", label,
             (unsigned int)packed);
      return NULL;
    }

  tlvlen = rpc_tlv_compose(tlv, sizeof(tlv), buf, (uint16_t)packed);
  if (tlvlen < 0)
    {
      return NULL;
    }

  printf("rpc: %s tx pb=%u tlv=%d\n", label, (unsigned int)packed, tlvlen);

    {
      size_t d;

      printf("rpc: %s pb bytes:", label);
      for (d = 0; d < packed && d < 80; d++)
        {
          printf(" %02x", buf[d]);
        }

      printf("\n");
    }

  cli->wait_uid = req->uid;
  cli->resp     = NULL;

  ret = esp_hosted_register(ESP_HOSTED_IF_SERIAL, rpc_rx_cb, NULL);
  if (ret < 0)
    {
      return NULL;
    }

  ret = esp_hosted_send(ESP_HOSTED_IF_SERIAL, 0, tlv, (uint16_t)tlvlen);
  if (ret < 0)
    {
      printf("rpc: %s send failed %d\n", label, ret);
      return NULL;
    }

  for (polls = 0; polls < RPC_RESP_WAIT_MS / RPC_POLL_INTERVAL_MS; polls++)
    {
      esp_hosted_poll();
      if (sem_trywait(&cli->resp_sem) == 0)
        {
          break;
        }

      usleep(RPC_POLL_INTERVAL_MS * 1000);
    }

  return cli->resp;
}

/****************************************************************************
 * Name: esp_hosted_rpc_wifi_init
 ****************************************************************************/

int esp_hosted_rpc_wifi_init(void)
{
  WifiInitConfig cfg = WIFI_INIT_CONFIG__INIT;
  RpcReqWifiInit req_payload = RPC__REQ__WIFI_INIT__INIT;
  Rpc req = { PROTOBUF_C_MESSAGE_INIT(&rpc__descriptor) };
  FAR Rpc *resp;
  int status;

  /* Values mirror WIFI_INIT_CONFIG_DEFAULT() as compiled by the reference
   * P4 host.  The slave uses every field the host sends (USE_HOST_VALUE), so
   * these must be the IDF defaults, not zeros.  magic is a hard requirement.
   */

  cfg.static_rx_buf_num       = 10;
  cfg.dynamic_rx_buf_num      = 32;
  cfg.tx_buf_type             = 1;   /* dynamic */
  cfg.static_tx_buf_num       = 0;
  cfg.dynamic_tx_buf_num      = 32;
  cfg.cache_tx_buf_num        = 0;
  cfg.csi_enable              = 0;
  cfg.ampdu_rx_enable         = 1;
  cfg.ampdu_tx_enable         = 1;
  cfg.amsdu_tx_enable         = 0;
  cfg.nvs_enable              = 1;
  cfg.nano_enable             = 0;
  cfg.rx_ba_win               = 6;
  cfg.wifi_task_core_id       = 0;
  cfg.beacon_max_len          = 752;
  cfg.mgmt_sbuf_num           = 32;
  cfg.feature_caps            = 0;
  cfg.sta_disconnected_pm     = 0;
  cfg.espnow_max_encrypt_num  = 7;
  cfg.magic                   = 0x1f2f3f4f;
  cfg.rx_mgmt_buf_type        = 1;   /* dynamic */
  cfg.rx_mgmt_buf_num         = 5;

  req.msg_type = RPC_TYPE__Req;
  req.msg_id   = RPC_ID__Req_WifiInit;
  req.uid      = 1;
  req.payload_case = RPC__PAYLOAD_REQ_WIFI_INIT;
  req.req_wifi_init = &req_payload;
  req_payload.cfg = &cfg;

  resp = rpc_transact(&req, "WifiInit");
  if (resp == NULL)
    {
      return -ETIMEDOUT;
    }

  status = resp->payload_case == RPC__PAYLOAD_RESP_WIFI_INIT ?
           resp->resp_wifi_init->resp : -1;
  printf("rpc: WifiInit resp=%d\n", status);
  rpc__free_unpacked(resp, NULL);
  return status == 0 ? OK : -EIO;
}

/****************************************************************************
 * Name: esp_hosted_rpc_wifi_set_mode
 ****************************************************************************/

int esp_hosted_rpc_wifi_set_mode(int mode)
{
  RpcReqSetMode req_payload = RPC__REQ__SET_MODE__INIT;
  Rpc req = { PROTOBUF_C_MESSAGE_INIT(&rpc__descriptor) };
  FAR Rpc *resp;
  int status;

  req.msg_type = RPC_TYPE__Req;
  req.msg_id   = RPC_ID__Req_SetWifiMode;
  req.uid      = 1;
  req.payload_case = RPC__PAYLOAD_REQ_SET_WIFI_MODE;
  req.req_set_wifi_mode = &req_payload;
  req_payload.mode = mode;

  resp = rpc_transact(&req, "SetMode");
  if (resp == NULL)
    {
      return -ETIMEDOUT;
    }

  status = resp->payload_case == RPC__PAYLOAD_RESP_SET_WIFI_MODE ?
           resp->resp_set_wifi_mode->resp : -1;
  printf("rpc: SetMode(%d) resp=%d\n", mode, status);
  rpc__free_unpacked(resp, NULL);
  return status == 0 ? OK : -EIO;
}

/****************************************************************************
 * Name: esp_hosted_rpc_wifi_start
 ****************************************************************************/

int esp_hosted_rpc_wifi_start(void)
{
  RpcReqWifiStart req_payload = RPC__REQ__WIFI_START__INIT;
  Rpc req = { PROTOBUF_C_MESSAGE_INIT(&rpc__descriptor) };
  FAR Rpc *resp;
  int status;

  req.msg_type = RPC_TYPE__Req;
  req.msg_id   = RPC_ID__Req_WifiStart;
  req.uid      = 1;
  req.payload_case = RPC__PAYLOAD_REQ_WIFI_START;
  req.req_wifi_start = &req_payload;

  resp = rpc_transact(&req, "WifiStart");
  if (resp == NULL)
    {
      return -ETIMEDOUT;
    }

  status = resp->payload_case == RPC__PAYLOAD_RESP_WIFI_START ?
           resp->resp_wifi_start->resp : -1;
  printf("rpc: WifiStart resp=%d\n", status);
  rpc__free_unpacked(resp, NULL);
  return status == 0 ? OK : -EIO;
}

/****************************************************************************
 * Name: esp_hosted_rpc_wifi_scan
 *
 * Description:
 *   Run a blocking all-channel scan and print what the C6 found.  Passing no
 *   scan config makes the slave call esp_wifi_scan_start(NULL), i.e. the
 *   default scan.
 *
 ****************************************************************************/

int esp_hosted_rpc_wifi_scan(void)
{
  RpcReqWifiScanStart start_payload = RPC__REQ__WIFI_SCAN_START__INIT;
  RpcReqWifiScanGetApNum num_payload = RPC__REQ__WIFI_SCAN_GET_AP_NUM__INIT;
  RpcReqWifiScanGetApRecords rec_payload =
    RPC__REQ__WIFI_SCAN_GET_AP_RECORDS__INIT;
  Rpc req = { PROTOBUF_C_MESSAGE_INIT(&rpc__descriptor) };
  FAR Rpc *resp;
  int status;
  int number;
  size_t i;

  /* Start: block until the scan finishes so no event handling is needed. */

  req.msg_type = RPC_TYPE__Req;
  req.msg_id   = RPC_ID__Req_WifiScanStart;
  req.uid      = 1;
  req.payload_case = RPC__PAYLOAD_REQ_WIFI_SCAN_START;
  req.req_wifi_scan_start = &start_payload;
  start_payload.block = true;
  start_payload.config = NULL;
  start_payload.config_set = 0;

  resp = rpc_transact(&req, "WifiScanStart");
  if (resp == NULL)
    {
      return -ETIMEDOUT;
    }

  status = resp->payload_case == RPC__PAYLOAD_RESP_WIFI_SCAN_START ?
           resp->resp_wifi_scan_start->resp : -1;
  printf("rpc: WifiScanStart resp=%d\n", status);
  rpc__free_unpacked(resp, NULL);
  if (status != 0)
    {
      return -EIO;
    }

  /* How many APs were found? */

    {
      Rpc numreq = { PROTOBUF_C_MESSAGE_INIT(&rpc__descriptor) };

      numreq.msg_type = RPC_TYPE__Req;
      numreq.msg_id   = RPC_ID__Req_WifiScanGetApNum;
      numreq.uid      = 1;
      numreq.payload_case = RPC__PAYLOAD_REQ_WIFI_SCAN_GET_AP_NUM;
      numreq.req_wifi_scan_get_ap_num = &num_payload;

      resp = rpc_transact(&numreq, "ScanGetApNum");
    }

  if (resp == NULL)
    {
      return -ETIMEDOUT;
    }

  if (resp->payload_case != RPC__PAYLOAD_RESP_WIFI_SCAN_GET_AP_NUM)
    {
      rpc__free_unpacked(resp, NULL);
      return -EIO;
    }

  status = resp->resp_wifi_scan_get_ap_num->resp;
  number = resp->resp_wifi_scan_get_ap_num->number;
  printf("rpc: ScanGetApNum resp=%d number=%d\n", status, number);
  rpc__free_unpacked(resp, NULL);

  if (status != 0 || number <= 0)
    {
      return status != 0 ? -EIO : OK;
    }

  /* Fetch the records.  Keep the batch small so the reply fits one frame. */

  if (number > 10)
    {
      number = 10;
    }

    {
      Rpc recreq = { PROTOBUF_C_MESSAGE_INIT(&rpc__descriptor) };

      recreq.msg_type = RPC_TYPE__Req;
      recreq.msg_id   = RPC_ID__Req_WifiScanGetApRecords;
      recreq.uid      = 1;
      recreq.payload_case = RPC__PAYLOAD_REQ_WIFI_SCAN_GET_AP_RECORDS;
      recreq.req_wifi_scan_get_ap_records = &rec_payload;
      rec_payload.number = number;

      resp = rpc_transact(&recreq, "ScanGetApRecords");
    }

  if (resp == NULL)
    {
      return -ETIMEDOUT;
    }

  if (resp->payload_case != RPC__PAYLOAD_RESP_WIFI_SCAN_GET_AP_RECORDS)
    {
      rpc__free_unpacked(resp, NULL);
      return -EIO;
    }

  printf("rpc: %u AP(s):\n",
         (unsigned int)resp->resp_wifi_scan_get_ap_records->n_ap_records);

  for (i = 0; i < resp->resp_wifi_scan_get_ap_records->n_ap_records; i++)
    {
      FAR WifiApRecord *ap =
        resp->resp_wifi_scan_get_ap_records->ap_records[i];

      printf("  ch%-3" PRIu32 " rssi=%-4" PRId32 " %.*s\n",
             ap->primary, ap->rssi, (int)ap->ssid.len,
             (FAR const char *)ap->ssid.data);
    }

  rpc__free_unpacked(resp, NULL);
  return OK;
}

/****************************************************************************
 * Name: esp_hosted_rpc_wifi_connect
 *
 * Description:
 *   Program the STA credentials and start association.  The slave treats the
 *   threshold and pmf sub-messages as optional, so only SSID and password are
 *   sent.  Credentials come from the caller, never from the build.
 *
 ****************************************************************************/

int esp_hosted_rpc_wifi_connect(FAR const char *ssid, FAR const char *pwd)
{
  uint8_t ssid_buf[32] = {0};
  uint8_t password_buf[64] = {0};
  WifiStaConfig sta = WIFI_STA_CONFIG__INIT;
  WifiScanThreshold threshold = WIFI_SCAN_THRESHOLD__INIT;
  WifiPmfConfig pmf_cfg = WIFI_PMF_CONFIG__INIT;
  WifiConfig cfg = WIFI_CONFIG__INIT;
  RpcReqWifiSetConfig set_payload = RPC__REQ__WIFI_SET_CONFIG__INIT;
  RpcReqWifiConnect conn_payload = RPC__REQ__WIFI_CONNECT__INIT;
  FAR Rpc *resp;
  int status;

  /* An empty SSID means "skip SetConfig and just associate", which is how the
   * caller can test whether the slave still has a working control path when
   * SetConfig itself misbehaves.
   */

  if (ssid != NULL && strlen(ssid) > 0)
    {
      if (strlen(ssid) > 32 || pwd == NULL || strlen(pwd) > 64)
        {
          return -EINVAL;
        }

      memcpy(ssid_buf, ssid, strlen(ssid));
      memcpy(password_buf, pwd, strlen(pwd));
      sta.ssid.data = ssid_buf;
      sta.ssid.len  = strlen(ssid) + 1;
      sta.password.data = password_buf;
      sta.password.len  = strlen(pwd) + 1;
      sta.threshold = &threshold;
      sta.pmf_cfg = &pmf_cfg;

      cfg.u_case = WIFI_CONFIG__U_STA;
      cfg.sta = &sta;

      set_payload.iface = 0;   /* WIFI_IF_STA */
      set_payload.cfg = &cfg;

        {
          Rpc req = { PROTOBUF_C_MESSAGE_INIT(&rpc__descriptor) };

          req.msg_type = RPC_TYPE__Req;
          req.msg_id   = RPC_ID__Req_WifiSetConfig;
          req.uid      = 1;
          req.payload_case = RPC__PAYLOAD_REQ_WIFI_SET_CONFIG;
          req.req_wifi_set_config = &set_payload;

          resp = rpc_transact(&req, "WifiSetConfig");
        }

      if (resp == NULL)
        {
          return -ETIMEDOUT;
        }

      status = resp->payload_case == RPC__PAYLOAD_RESP_WIFI_SET_CONFIG ?
               resp->resp_wifi_set_config->resp : -1;
      printf("rpc: WifiSetConfig resp=%d\n", status);
      rpc__free_unpacked(resp, NULL);
      if (status != 0)
        {
          return -EIO;
        }
    }
  else
    {
      printf("rpc: skipping SetConfig, using slave's stored config\n");
    }

    {
      Rpc req = { PROTOBUF_C_MESSAGE_INIT(&rpc__descriptor) };

      req.msg_type = RPC_TYPE__Req;
      req.msg_id   = RPC_ID__Req_WifiConnect;
      req.uid      = 1;
      req.payload_case = RPC__PAYLOAD_REQ_WIFI_CONNECT;
      req.req_wifi_connect = &conn_payload;

      resp = rpc_transact(&req, "WifiConnect");
    }

  if (resp == NULL)
    {
      return -ETIMEDOUT;
    }

  status = resp->payload_case == RPC__PAYLOAD_RESP_WIFI_CONNECT ?
           resp->resp_wifi_connect->resp : -1;
  printf("rpc: WifiConnect resp=%d\n", status);
  rpc__free_unpacked(resp, NULL);
  return status == 0 ? OK : -EIO;
}

/****************************************************************************
 * Name: esp_hosted_rpc_get_mac
 *
 * Description:
 *   Send the GetMacAddress RPC for one interface and wait for the reply.
 *
 * Input Parameters:
 *   wifi_if - 0 for STA, 1 for AP
 *   mac     - Buffer receiving the six MAC address bytes
 *
 * Returned Value:
 *   OK on success; a negated errno on failure.
 *
 ****************************************************************************/

int esp_hosted_rpc_get_mac(int wifi_if, uint8_t mac[6])
{
  FAR struct rpc_client_s *cli = &g_rpc;
  RpcReqGetMacAddress req_payload = RPC__REQ__GET_MAC_ADDRESS__INIT;
  Rpc req = { PROTOBUF_C_MESSAGE_INIT(&rpc__descriptor) };
  FAR Rpc *resp;

  if (wifi_if != RPC_WIFI_IF_STA && wifi_if != RPC_WIFI_IF_AP)
    {
      return -EINVAL;
    }

  req.msg_type = RPC_TYPE__Req;
  req.msg_id   = RPC_ID__Req_GetMACAddress;
  req.uid      = 1;
  req.payload_case = RPC__PAYLOAD_REQ_GET_MAC_ADDRESS;
  req.req_get_mac_address = &req_payload;
  req_payload.mode = wifi_if;

  resp = rpc_transact(&req, "GetMacAddress");
  if (resp == NULL)
    {
      cli->busy = false;
      return -ETIMEDOUT;
    }

  cli->busy = false;

  if (resp->payload_case == RPC__PAYLOAD_RESP_GET_MAC_ADDRESS &&
      resp->resp_get_mac_address->mac.len == 6)
    {
      memcpy(mac, resp->resp_get_mac_address->mac.data, 6);
      rpc__free_unpacked(resp, NULL);
      return OK;
    }

  printf("rpc: GetMacAddress resp code=%d mac.len=%u\n",
         resp->payload_case == RPC__PAYLOAD_RESP_GET_MAC_ADDRESS ?
         resp->resp_get_mac_address->resp : -1,
         resp->payload_case == RPC__PAYLOAD_RESP_GET_MAC_ADDRESS ?
         (unsigned int)resp->resp_get_mac_address->mac.len : 0);

  rpc__free_unpacked(resp, NULL);
  return -EIO;
}
