#include "proto_control_udp.h"

#include "boot_metadata.h"
#include "proto_common.h"
#include "resident_device_tree.h"
#include "resident_security.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "cmsis_os.h"

#include <string.h>

typedef struct __attribute__((packed))
{
  uint32_t uptime_ms;
  uint32_t resident_version;
  uint32_t app_version;
} PingReplyPayload;

typedef struct __attribute__((packed))
{
  uint16_t prog_port;
  uint16_t reserved;
} ProgBeginReplyPayload;

typedef struct __attribute__((packed))
{
  int16_t result;
  uint16_t detail;
} ErrorReplyPayload;

typedef struct __attribute__((packed))
{
  uint8_t op;
  uint8_t node_depth;
} DeviceTreeRequestPrefix;

#define DEVICETREE_OP_LIST             (0x01U)
#define DEVICETREE_OP_GET              (0x02U)
#define DEVICETREE_OP_SET              (0x03U)
#define DEVICETREE_OP_EXECUTE          (0x04U)
#define DEVICETREE_OP_UNLOCK           (0x05U)
#define DEVICE_UID_BYTES               (12U)
#define BCAST_UID_REQ_MIN_LEN          (DEVICE_UID_BYTES + 1U + sizeof(uint16_t))
#define BCAST_UID_REPLY_OVERHEAD       (DEVICE_UID_BYTES + 1U + sizeof(uint16_t))
/* Device-tree LIST payloads (JSON per row) exceed 512B for /Debug/Flash; keep outer reply under LAN MTU headroom */
#define CONTROL_REPLY_PAYLOAD_MAX      (1536U)
#define TREE_RESPONSE_MAX              (1488U)

_Static_assert(CONTROL_REPLY_PAYLOAD_MAX >= TREE_RESPONSE_MAX + 2U + 16U + sizeof(int16_t) + sizeof(uint16_t),
               "DEVICE_TREE_REPLY must accommodate max-depth prefix + TREE_RESPONSE_MAX payload");

extern struct netif gnetif;

static struct udp_pcb *g_control_pcb;
static uint8_t g_command_reply_payload[CONTROL_REPLY_PAYLOAD_MAX];
static uint8_t g_inner_reply_payload[CONTROL_REPLY_PAYLOAD_MAX];
static uint8_t g_outer_reply_payload[BCAST_UID_REPLY_OVERHEAD + CONTROL_REPLY_PAYLOAD_MAX];
static uint8_t g_tree_response[TREE_RESPONSE_MAX];

static void read_uid(uint8_t uid[DEVICE_UID_BYTES])
{
#ifdef UID_BASE
  memcpy(uid, (const void *)UID_BASE, DEVICE_UID_BYTES);
#else
  memset(uid, 0, DEVICE_UID_BYTES);
#endif
}

static const ip_addr_t *reply_address_for_source(const ip_addr_t *addr)
{
  if ((addr != 0) && IP_IS_V4(addr) &&
      !ip4_addr_netcmp(ip_2_ip4(addr), netif_ip4_addr(&gnetif), netif_ip4_netmask(&gnetif)))
  {
    return IP_ADDR_BROADCAST;
  }

  return addr;
}

static void send_reply(struct udp_pcb *pcb, const ip_addr_t *addr, u16_t port,
                       const ProtoCommandHeader *request, ProtoMessageType type,
                       const void *payload, uint16_t payload_len)
{
  const uint16_t total_len = sizeof(ProtoCommandHeader) + payload_len;
  struct pbuf *response = pbuf_alloc(PBUF_TRANSPORT, total_len, PBUF_RAM);
  if (response == 0)
  {
    return;
  }

  ProtoCommandHeader header;
  memset(&header, 0, sizeof(header));
  header.magic = PROTO_MAGIC;
  header.proto_version = PROTO_VERSION;
  header.msg_type = (uint8_t)type;
  header.flags = PROTO_FLAG_REPLY;
  header.transaction_id = request->transaction_id;
  header.payload_len = payload_len;

  memcpy(response->payload, &header, sizeof(header));
  if ((payload != 0) && (payload_len != 0U))
  {
    memcpy((uint8_t *)response->payload + sizeof(header), payload, payload_len);
  }

  (void)udp_sendto(pcb, response, reply_address_for_source(addr), port);
  pbuf_free(response);
}

static void send_error(struct udp_pcb *pcb, const ip_addr_t *addr, u16_t port,
                       const ProtoCommandHeader *request, ProtoResult result, uint16_t detail)
{
  ErrorReplyPayload payload;
  payload.result = (int16_t)result;
  payload.detail = detail;
  send_reply(pcb, addr, port, request, PROTO_MSG_ERROR_REPLY, &payload, sizeof(payload));
}

static int build_error_payload(uint8_t *payload, uint16_t payload_max, ProtoResult result,
                               uint16_t detail, uint16_t *payload_len)
{
  ErrorReplyPayload error_payload;
  if ((payload == 0) || (payload_len == 0) || (payload_max < sizeof(error_payload)))
  {
    return PROTO_RESULT_GENERIC;
  }

  error_payload.result = (int16_t)result;
  error_payload.detail = detail;
  memcpy(payload, &error_payload, sizeof(error_payload));
  *payload_len = sizeof(error_payload);
  return PROTO_RESULT_OK;
}

static int build_device_tree_reply(const uint8_t *request_payload, uint16_t request_payload_len,
                                   uint8_t *reply_payload, uint16_t reply_payload_max,
                                   uint16_t *reply_payload_len)
{
  uint16_t offset = 0U;
  uint16_t op_payload_len;
  uint16_t tree_response_len = 0U;
  int16_t result = PROTO_RESULT_OK;

  if ((request_payload == 0) || (reply_payload == 0) || (reply_payload_len == 0) ||
      (request_payload_len < sizeof(DeviceTreeRequestPrefix)))
  {
    return PROTO_RESULT_PARSE;
  }

  const DeviceTreeRequestPrefix *prefix = (const DeviceTreeRequestPrefix *)request_payload;
  const uint8_t node_depth = prefix->node_depth;
  const uint8_t *node_location = request_payload + sizeof(DeviceTreeRequestPrefix);
  if ((node_depth > 16U) ||
      (request_payload_len < (uint16_t)(sizeof(DeviceTreeRequestPrefix) + node_depth + sizeof(uint16_t))))
  {
    return PROTO_RESULT_PARSE;
  }

  const uint16_t payload_len_offset = (uint16_t)(sizeof(DeviceTreeRequestPrefix) + node_depth);
  memcpy(&op_payload_len, request_payload + payload_len_offset, sizeof(op_payload_len));
  const uint8_t *op_payload = request_payload + payload_len_offset + sizeof(uint16_t);
  if (request_payload_len < (uint16_t)(payload_len_offset + sizeof(uint16_t) + op_payload_len))
  {
    return PROTO_RESULT_PARSE;
  }

  if ((2U + node_depth + sizeof(result) + sizeof(tree_response_len)) > reply_payload_max)
  {
    return PROTO_RESULT_GENERIC;
  }

  reply_payload[offset++] = prefix->op;
  reply_payload[offset++] = node_depth;
  if (node_depth != 0U)
  {
    memcpy(reply_payload + offset, node_location, node_depth);
    offset += node_depth;
  }

  switch (prefix->op)
  {
    case DEVICETREE_OP_LIST:
      result = (int16_t)resident_device_tree_list(node_location, node_depth, g_tree_response,
                                                  sizeof(g_tree_response), &tree_response_len);
      break;

    case DEVICETREE_OP_GET:
      result = (int16_t)resident_device_tree_get(node_location, node_depth, g_tree_response,
                                                 sizeof(g_tree_response), &tree_response_len);
      break;

    case DEVICETREE_OP_SET:
    {
      uint16_t value_len;
      if (op_payload_len < sizeof(value_len))
      {
        result = PROTO_RESULT_PARSE;
        break;
      }

      memcpy(&value_len, op_payload, sizeof(value_len));
      if (op_payload_len < (uint16_t)(sizeof(value_len) + value_len))
      {
        result = PROTO_RESULT_PARSE;
        break;
      }

      result = (int16_t)resident_device_tree_set(node_location, node_depth,
                                                 op_payload + sizeof(value_len), value_len,
                                                 g_tree_response, sizeof(g_tree_response),
                                                 &tree_response_len);
      break;
    }

    case DEVICETREE_OP_UNLOCK:
    {
      if (op_payload_len < 2U)
      {
        result = PROTO_RESULT_PARSE;
        break;
      }

      const uint8_t password_len = op_payload[1];
      if (op_payload_len < (uint16_t)(2U + password_len))
      {
        result = PROTO_RESULT_PARSE;
        break;
      }

      const char expected_password[] = "password";
      if ((password_len == (sizeof(expected_password) - 1U)) &&
          (memcmp(&op_payload[2], expected_password, sizeof(expected_password) - 1U) == 0))
      {
        g_tree_response[0] = 0x41U;
        tree_response_len = 1U;
        resident_security_set_programming_unlocked(true, 0U);
      }
      else
      {
        result = PROTO_RESULT_UNAUTHORIZED;
      }
      break;
    }

    case DEVICETREE_OP_EXECUTE:
    {
      uint16_t args_len;
      if (op_payload_len < sizeof(args_len))
      {
        result = PROTO_RESULT_PARSE;
        break;
      }

      memcpy(&args_len, op_payload, sizeof(args_len));
      if (op_payload_len < (uint16_t)(sizeof(args_len) + args_len))
      {
        result = PROTO_RESULT_PARSE;
        break;
      }

      result = (int16_t)resident_device_tree_execute(node_location, node_depth,
                                                     op_payload + sizeof(args_len), args_len,
                                                     g_tree_response, sizeof(g_tree_response),
                                                     &tree_response_len);
      break;
    }

    default:
      result = PROTO_RESULT_NOT_FOUND;
      break;
  }

  if ((offset + sizeof(result) + sizeof(tree_response_len) + tree_response_len) > reply_payload_max)
  {
    return PROTO_RESULT_GENERIC;
  }

  memcpy(reply_payload + offset, &result, sizeof(result));
  offset += sizeof(result);
  memcpy(reply_payload + offset, &tree_response_len, sizeof(tree_response_len));
  offset += sizeof(tree_response_len);
  if (tree_response_len != 0U)
  {
    memcpy(reply_payload + offset, g_tree_response, tree_response_len);
    offset += tree_response_len;
  }

  *reply_payload_len = offset;
  return PROTO_RESULT_OK;
}

static ProtoMessageType build_command_reply(ProtoMessageType request_type,
                                            const uint8_t *request_payload,
                                            uint16_t request_payload_len,
                                            uint8_t *reply_payload,
                                            uint16_t reply_payload_max,
                                            uint16_t *reply_payload_len)
{
  switch (request_type)
  {
    case PROTO_MSG_PING_REQ:
    {
      PingReplyPayload payload;
      if (reply_payload_max < sizeof(payload))
      {
        (void)build_error_payload(reply_payload, reply_payload_max, PROTO_RESULT_GENERIC, 0U,
                                  reply_payload_len);
        return PROTO_MSG_ERROR_REPLY;
      }

      payload.uptime_ms = osKernelGetTickCount();
      payload.resident_version = 1U;
      payload.app_version = 0U;
      (void)boot_metadata_kv_read_u32(BOOT_KV_APP_VERSION, &payload.app_version);
      memcpy(reply_payload, &payload, sizeof(payload));
      *reply_payload_len = sizeof(payload);
      return PROTO_MSG_PING_REPLY;
    }

    case PROTO_MSG_PROG_BEGIN_REQ:
    {
      ProgBeginReplyPayload payload;
      if (!resident_security_programming_is_unlocked())
      {
        (void)build_error_payload(reply_payload, reply_payload_max, PROTO_RESULT_LOCKED, 0U,
                                  reply_payload_len);
        return PROTO_MSG_ERROR_REPLY;
      }

      if (reply_payload_max < sizeof(payload))
      {
        (void)build_error_payload(reply_payload, reply_payload_max, PROTO_RESULT_GENERIC, 0U,
                                  reply_payload_len);
        return PROTO_MSG_ERROR_REPLY;
      }

      payload.prog_port = PROG_PORT;
      payload.reserved = 0U;
      memcpy(reply_payload, &payload, sizeof(payload));
      *reply_payload_len = sizeof(payload);
      return PROTO_MSG_PROG_BEGIN_REPLY;
    }

    case PROTO_MSG_DEVICETREE_REQ:
    {
      const int result = build_device_tree_reply(request_payload, request_payload_len,
                                                reply_payload, reply_payload_max,
                                                reply_payload_len);
      if (result != PROTO_RESULT_OK)
      {
        (void)build_error_payload(reply_payload, reply_payload_max, (ProtoResult)result, 0U,
                                  reply_payload_len);
        return PROTO_MSG_ERROR_REPLY;
      }
      return PROTO_MSG_DEVICETREE_REPLY;
    }

    default:
      (void)build_error_payload(reply_payload, reply_payload_max, PROTO_RESULT_PARSE, 0U,
                                reply_payload_len);
      return PROTO_MSG_ERROR_REPLY;
  }
}

static void handle_broadcast_uid_request(struct udp_pcb *pcb, const ip_addr_t *addr, u16_t port,
                                         const ProtoCommandHeader *request,
                                         const uint8_t *request_payload)
{
  uint8_t uid[DEVICE_UID_BYTES];
  uint16_t inner_len;
  uint16_t inner_reply_len = 0U;
  uint16_t outer_reply_len;

  if (request->payload_len < BCAST_UID_REQ_MIN_LEN)
  {
    send_error(pcb, addr, port, request, PROTO_RESULT_PARSE, 0U);
    return;
  }

  read_uid(uid);
  if (memcmp(request_payload, uid, DEVICE_UID_BYTES) != 0)
  {
    return;
  }

  const uint8_t inner_msg_type = request_payload[DEVICE_UID_BYTES];
  memcpy(&inner_len, request_payload + DEVICE_UID_BYTES + 1U, sizeof(inner_len));
  const uint8_t *inner_payload = request_payload + BCAST_UID_REQ_MIN_LEN;
  if (request->payload_len < (uint16_t)(BCAST_UID_REQ_MIN_LEN + inner_len))
  {
    send_error(pcb, addr, port, request, PROTO_RESULT_PARSE, 1U);
    return;
  }

  const ProtoMessageType inner_reply_type = build_command_reply((ProtoMessageType)inner_msg_type,
                                                               inner_payload, inner_len,
                                                               g_inner_reply_payload,
                                                               sizeof(g_inner_reply_payload),
                                                               &inner_reply_len);
  if (inner_reply_len > CONTROL_REPLY_PAYLOAD_MAX)
  {
    send_error(pcb, addr, port, request, PROTO_RESULT_GENERIC, 2U);
    return;
  }

  memcpy(g_outer_reply_payload, uid, DEVICE_UID_BYTES);
  g_outer_reply_payload[DEVICE_UID_BYTES] = (uint8_t)inner_reply_type;
  memcpy(g_outer_reply_payload + DEVICE_UID_BYTES + 1U, &inner_reply_len, sizeof(inner_reply_len));
  memcpy(g_outer_reply_payload + BCAST_UID_REPLY_OVERHEAD, g_inner_reply_payload, inner_reply_len);
  outer_reply_len = (uint16_t)(BCAST_UID_REPLY_OVERHEAD + inner_reply_len);
  send_reply(pcb, addr, port, request, PROTO_MSG_BCAST_UID_REPLY, g_outer_reply_payload, outer_reply_len);
}

static void control_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                         const ip_addr_t *addr, u16_t port)
{
  (void)arg;

  if ((p == 0) || (p->len < sizeof(ProtoCommandHeader)))
  {
    if (p != 0)
    {
      pbuf_free(p);
    }
    return;
  }

  const ProtoCommandHeader *request = (const ProtoCommandHeader *)p->payload;
  if ((request->magic != PROTO_MAGIC) || (request->proto_version != PROTO_VERSION))
  {
    pbuf_free(p);
    return;
  }

  const uint16_t available_payload_len = (uint16_t)(p->len - sizeof(ProtoCommandHeader));
  if (request->payload_len > available_payload_len)
  {
    send_error(pcb, addr, port, request, PROTO_RESULT_PARSE, 0U);
    pbuf_free(p);
    return;
  }

  const uint8_t *request_payload = (const uint8_t *)p->payload + sizeof(ProtoCommandHeader);

  if ((ProtoMessageType)request->msg_type == PROTO_MSG_BCAST_UID_REQ)
  {
    handle_broadcast_uid_request(pcb, addr, port, request, request_payload);
  }
  else
  {
    uint16_t reply_payload_len = 0U;
    const ProtoMessageType reply_type = build_command_reply((ProtoMessageType)request->msg_type,
                                                            request_payload,
                                                            request->payload_len,
                                                            g_command_reply_payload,
                                                            sizeof(g_command_reply_payload),
                                                            &reply_payload_len);
    send_reply(pcb, addr, port, request, reply_type, g_command_reply_payload, reply_payload_len);
  }

  pbuf_free(p);
}

int proto_control_udp_start(void)
{
  g_control_pcb = udp_new();
  if (g_control_pcb == 0)
  {
    return -1;
  }

  if (udp_bind(g_control_pcb, IP_ADDR_ANY, CONTROL_PORT) != ERR_OK)
  {
    udp_remove(g_control_pcb);
    g_control_pcb = 0;
    return -1;
  }

  udp_recv(g_control_pcb, control_recv, 0);
  return 0;
}
