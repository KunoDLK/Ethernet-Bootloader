#include "proto_program_tcp.h"

#include "app_image.h"
#include "boot_app_manager.h"
#include "boot_flash.h"
#include "boot_image.h"
#include "boot_metadata.h"
#include "proto_common.h"
#include "resident_security.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include <string.h>

typedef enum
{
  PROG_FRAME_HELLO = 0x01,
  PROG_FRAME_DATA = 0x02,
  PROG_FRAME_FINISH = 0x03,
  PROG_FRAME_ABORT = 0x04,
  PROG_FRAME_ACK = 0x81,
  PROG_FRAME_NACK = 0x82,
} ProgFrameType;

typedef struct __attribute__((packed))
{
  uint32_t image_size;
  uint8_t image_sha1[APP_IMAGE_SHA1_DIGEST_BYTES];
} ProgHelloPayload;

typedef struct __attribute__((packed))
{
  uint16_t data_len;
  uint8_t data[];
} ProgDataPayload;

typedef struct __attribute__((packed))
{
  uint32_t total_bytes;
  uint8_t final_sha1[APP_IMAGE_SHA1_DIGEST_BYTES];
} ProgFinishPayload;

typedef struct __attribute__((packed))
{
  uint32_t ack_seq;
  uint32_t bytes_written;
} ProgAckPayload;

typedef struct __attribute__((packed))
{
  uint32_t nack_seq;
  int16_t error_code;
  uint16_t detail;
} ProgNackPayload;

static struct tcp_pcb *g_listen_pcb;
static struct tcp_pcb *g_client_pcb;
static uint32_t g_expected_seq;
static uint32_t g_bytes_written;
static uint32_t g_expected_image_size;

static err_t send_ack(struct tcp_pcb *pcb, uint32_t seq)
{
  ProtoProgFrameHeader header;
  ProgAckPayload payload;
  uint8_t frame[sizeof(header) + sizeof(payload)];

  memset(&header, 0, sizeof(header));
  header.magic = PROTO_MAGIC;
  header.proto_version = PROTO_VERSION;
  header.frame_type = PROG_FRAME_ACK;
  header.flags = PROTO_FLAG_REPLY;
  header.seq = seq;
  header.payload_len = sizeof(payload);

  payload.ack_seq = seq;
  payload.bytes_written = g_bytes_written;

  memcpy(frame, &header, sizeof(header));
  memcpy(frame + sizeof(header), &payload, sizeof(payload));
  return tcp_write(pcb, frame, sizeof(frame), TCP_WRITE_FLAG_COPY);
}

static err_t send_nack(struct tcp_pcb *pcb, uint32_t seq, int16_t error_code, uint16_t detail)
{
  ProtoProgFrameHeader header;
  ProgNackPayload payload;
  uint8_t frame[sizeof(header) + sizeof(payload)];

  memset(&header, 0, sizeof(header));
  header.magic = PROTO_MAGIC;
  header.proto_version = PROTO_VERSION;
  header.frame_type = PROG_FRAME_NACK;
  header.flags = PROTO_FLAG_REPLY;
  header.seq = seq;
  header.payload_len = sizeof(payload);

  payload.nack_seq = seq;
  payload.error_code = error_code;
  payload.detail = detail;

  memcpy(frame, &header, sizeof(header));
  memcpy(frame + sizeof(header), &payload, sizeof(payload));
  return tcp_write(pcb, frame, sizeof(frame), TCP_WRITE_FLAG_COPY);
}

static err_t program_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
  (void)arg;

  if ((err != ERR_OK) || (p == 0))
  {
    g_client_pcb = 0;
    return ERR_OK;
  }

  tcp_recved(pcb, p->tot_len);

  if (p->len < sizeof(ProtoProgFrameHeader))
  {
    (void)send_nack(pcb, g_expected_seq, -2, 0U);
    pbuf_free(p);
    return ERR_OK;
  }

  const ProtoProgFrameHeader *header = (const ProtoProgFrameHeader *)p->payload;
  const uint8_t *payload = (const uint8_t *)p->payload + sizeof(ProtoProgFrameHeader);

  if ((header->magic != PROTO_MAGIC) ||
      (header->proto_version != PROTO_VERSION) ||
      (header->seq != g_expected_seq) ||
      ((sizeof(ProtoProgFrameHeader) + header->payload_len) > p->len))
  {
    (void)send_nack(pcb, header->seq, -4, 0U);
    pbuf_free(p);
    return ERR_OK;
  }

  switch ((ProgFrameType)header->frame_type)
  {
    case PROG_FRAME_HELLO:
    {
      if (!resident_security_programming_is_unlocked() ||
          (header->payload_len < sizeof(ProgHelloPayload)))
      {
        (void)send_nack(pcb, header->seq, -2, 0U);
        break;
      }

      const ProgHelloPayload *hello = (const ProgHelloPayload *)payload;
      g_expected_image_size = hello->image_size;
      g_bytes_written = 0U;
      (void)boot_app_manager_stop();
      if (boot_flash_erase_app_storage() != 0)
      {
        (void)send_nack(pcb, header->seq, -5, 0U);
        break;
      }

      (void)send_ack(pcb, header->seq);
      g_expected_seq++;
      break;
    }

    case PROG_FRAME_DATA:
    {
      if (header->payload_len < sizeof(uint16_t))
      {
        (void)send_nack(pcb, header->seq, -3, 0U);
        break;
      }

      const ProgDataPayload *data = (const ProgDataPayload *)payload;
      if ((data->data_len == 0U) || (data->data_len > 1000U) ||
          ((sizeof(uint16_t) + data->data_len) > header->payload_len))
      {
        (void)send_nack(pcb, header->seq, -3, 0U);
        break;
      }

      if (boot_flash_write_app_storage(g_bytes_written, data->data, data->data_len) != 0)
      {
        (void)send_nack(pcb, header->seq, -5, 0U);
        break;
      }

      g_bytes_written += data->data_len;
      (void)send_ack(pcb, header->seq);
      g_expected_seq++;
      break;
    }

    case PROG_FRAME_FINISH:
    {
      if (header->payload_len < sizeof(ProgFinishPayload))
      {
        (void)send_nack(pcb, header->seq, -3, 0U);
        break;
      }

      const ProgFinishPayload *finish = (const ProgFinishPayload *)payload;
      if ((finish->total_bytes != g_bytes_written) ||
          (g_expected_image_size != 0U && finish->total_bytes != g_expected_image_size))
      {
        (void)send_nack(pcb, header->seq, -6, 0U);
        break;
      }

      AppImageHeader image_header;
      if ((boot_image_read_header(&image_header) != BOOT_IMAGE_OK) ||
          (boot_image_validate(&image_header, true) != BOOT_IMAGE_OK))
      {
        (void)send_nack(pcb, header->seq, -6, 0U);
        break;
      }

      (void)boot_metadata_set_app_valid(image_header.app_version);
      (void)send_ack(pcb, header->seq);
      g_expected_seq++;
      break;
    }

    case PROG_FRAME_ABORT:
      (void)send_ack(pcb, header->seq);
      tcp_close(pcb);
      g_client_pcb = 0;
      break;

    default:
      (void)send_nack(pcb, header->seq, -2, 0U);
      break;
  }

  pbuf_free(p);
  return ERR_OK;
}

static void program_err(void *arg, err_t err)
{
  (void)arg;
  (void)err;
  g_client_pcb = 0;
}

static err_t program_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
  (void)arg;
  if (err != ERR_OK)
  {
    return err;
  }

  if (g_client_pcb != 0)
  {
    return ERR_ABRT;
  }

  g_client_pcb = newpcb;
  g_expected_seq = 0U;
  g_bytes_written = 0U;
  g_expected_image_size = 0U;
  tcp_recv(newpcb, program_recv);
  tcp_err(newpcb, program_err);
  return ERR_OK;
}

int proto_program_tcp_start(void)
{
  struct tcp_pcb *pcb = tcp_new();
  if (pcb == 0)
  {
    return -1;
  }

  if (tcp_bind(pcb, IP_ADDR_ANY, PROG_PORT) != ERR_OK)
  {
    tcp_close(pcb);
    return -1;
  }

  g_listen_pcb = tcp_listen(pcb);
  if (g_listen_pcb == 0)
  {
    tcp_close(pcb);
    return -1;
  }

  tcp_accept(g_listen_pcb, program_accept);
  return 0;
}

void proto_program_tcp_poll(void)
{
}
