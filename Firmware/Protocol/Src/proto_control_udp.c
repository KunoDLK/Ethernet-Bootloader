#include "proto_control_udp.h"

#include "boot_metadata.h"
#include "proto_common.h"
#include "resident_security.h"
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

#define DEVICETREE_OP_EXECUTE          (0x04U)
#define DEVICETREE_OP_UNLOCK           (0x05U)
#define DEVICETREE_UNLOCK_TIMEOUT_MS   (60000U)

static struct udp_pcb *g_control_pcb;

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

  (void)udp_sendto(pcb, response, addr, port);
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

  switch ((ProtoMessageType)request->msg_type)
  {
    case PROTO_MSG_PING_REQ:
    {
      PingReplyPayload payload;
      payload.uptime_ms = osKernelGetTickCount();
      payload.resident_version = 1U;
      payload.app_version = boot_metadata_get()->app_version;
      send_reply(pcb, addr, port, request, PROTO_MSG_PING_REPLY, &payload, sizeof(payload));
      break;
    }

    case PROTO_MSG_PROG_BEGIN_REQ:
    {
      if (!resident_security_programming_is_unlocked())
      {
        send_error(pcb, addr, port, request, PROTO_RESULT_LOCKED, 0U);
        break;
      }

      ProgBeginReplyPayload payload;
      payload.prog_port = PROG_PORT;
      payload.reserved = 0U;
      send_reply(pcb, addr, port, request, PROTO_MSG_PROG_BEGIN_REPLY, &payload, sizeof(payload));
      break;
    }

    case PROTO_MSG_DEVICETREE_REQ:
    {
      if (request->payload_len < sizeof(DeviceTreeRequestPrefix))
      {
        send_error(pcb, addr, port, request, PROTO_RESULT_PARSE, 0U);
        break;
      }

      const uint8_t *request_payload = (const uint8_t *)p->payload + sizeof(ProtoCommandHeader);
      const DeviceTreeRequestPrefix *prefix = (const DeviceTreeRequestPrefix *)request_payload;
      const uint32_t node_bytes = prefix->node_depth;
      if (node_bytes > 16U)
      {
        send_error(pcb, addr, port, request, PROTO_RESULT_PARSE, 5U);
        break;
      }

      const uint32_t payload_len_offset = sizeof(DeviceTreeRequestPrefix) + node_bytes;
      if (request->payload_len < (payload_len_offset + sizeof(uint16_t)))
      {
        send_error(pcb, addr, port, request, PROTO_RESULT_PARSE, 1U);
        break;
      }

      uint16_t op_payload_len;
      memcpy(&op_payload_len, request_payload + payload_len_offset, sizeof(op_payload_len));
      const uint8_t *op_payload = request_payload + payload_len_offset + sizeof(uint16_t);
      if (request->payload_len < (payload_len_offset + sizeof(uint16_t) + op_payload_len))
      {
        send_error(pcb, addr, port, request, PROTO_RESULT_PARSE, 2U);
        break;
      }

      if (prefix->op == DEVICETREE_OP_UNLOCK)
      {
        if (op_payload_len < 2U)
        {
          send_error(pcb, addr, port, request, PROTO_RESULT_PARSE, 3U);
          break;
        }

        const uint8_t password_len = op_payload[1];
        if (op_payload_len < (uint16_t)(2U + password_len))
        {
          send_error(pcb, addr, port, request, PROTO_RESULT_PARSE, 4U);
          break;
        }

        const char expected_password[] = "password";
        if ((password_len == (sizeof(expected_password) - 1U)) &&
            (memcmp(&op_payload[2], expected_password, sizeof(expected_password) - 1U) == 0))
        {
          uint8_t reply_payload[32];
          uint8_t *out = reply_payload;
          *out++ = prefix->op;
          *out++ = prefix->node_depth;
          memcpy(out, request_payload + sizeof(DeviceTreeRequestPrefix), node_bytes);
          out += node_bytes;
          int16_t result = PROTO_RESULT_OK;
          uint16_t response_len = 1U;
          uint8_t access_result = 0x41U;
          memcpy(out, &result, sizeof(result));
          out += sizeof(result);
          memcpy(out, &response_len, sizeof(response_len));
          out += sizeof(response_len);
          *out = access_result;
          resident_security_set_programming_unlocked(true, DEVICETREE_UNLOCK_TIMEOUT_MS);
          send_reply(pcb, addr, port, request, PROTO_MSG_DEVICETREE_REPLY, reply_payload,
                     (uint16_t)(sizeof(DeviceTreeRequestPrefix) + node_bytes + sizeof(result) + sizeof(response_len) + sizeof(access_result)));
        }
        else
        {
          send_error(pcb, addr, port, request, PROTO_RESULT_UNAUTHORIZED, 0U);
        }
      }
      else if (prefix->op == DEVICETREE_OP_EXECUTE)
      {
        (void)boot_metadata_enable_app();
        uint8_t reply_payload[32];
        uint8_t *out = reply_payload;
        *out++ = prefix->op;
        *out++ = prefix->node_depth;
        memcpy(out, request_payload + sizeof(DeviceTreeRequestPrefix), node_bytes);
        out += node_bytes;
        int16_t result = PROTO_RESULT_OK;
        uint16_t response_len = 0U;
        memcpy(out, &result, sizeof(result));
        out += sizeof(result);
        memcpy(out, &response_len, sizeof(response_len));
        send_reply(pcb, addr, port, request, PROTO_MSG_DEVICETREE_REPLY, reply_payload,
                   (uint16_t)(sizeof(DeviceTreeRequestPrefix) + node_bytes + sizeof(result) + sizeof(response_len)));
      }
      else
      {
        send_error(pcb, addr, port, request, PROTO_RESULT_NOT_FOUND, 0U);
      }
      break;
    }

    default:
      send_error(pcb, addr, port, request, PROTO_RESULT_PARSE, 0U);
      break;
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
