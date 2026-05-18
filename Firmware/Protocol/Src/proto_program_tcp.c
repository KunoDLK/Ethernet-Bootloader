#include "proto_program_tcp.h"

#include "app_image.h"
#include "boot_app_manager.h"
#include "boot_flash.h"
#include "boot_image.h"
#include "boot_metadata.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "proto_common.h"
#include "resident_program_manager.h"
#include "resident_security.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"

#include <stdio.h>
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

#define PROG_FLAG_RAW_STORAGE           (1U << 1)

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

#define PROGRAM_TCP_RX_BUFFER_BYTES     (2048U)

static struct tcp_pcb *g_listen_pcb;
static struct tcp_pcb *g_client_pcb;
static uint16_t g_listen_port;
static uint32_t g_expected_seq;
static uint32_t g_bytes_written;
static uint32_t g_expected_image_size;
static uint16_t g_program_flags;
static uint8_t g_flash_programming_active;
static uint8_t g_stop_after_final_ack;
static uint8_t g_rx_buffer[PROGRAM_TCP_RX_BUFFER_BYTES];
static uint16_t g_rx_buffer_len;
static StaticSemaphore_t g_start_sem_cb;
static StaticSemaphore_t g_stop_sem_cb;
static const osSemaphoreAttr_t g_start_sem_attr = {
  .name = "ProgTcpStart",
  .cb_mem = &g_start_sem_cb,
  .cb_size = sizeof(g_start_sem_cb),
};
static const osSemaphoreAttr_t g_stop_sem_attr = {
  .name = "ProgTcpStop",
  .cb_mem = &g_stop_sem_cb,
  .cb_size = sizeof(g_stop_sem_cb),
};

static err_t program_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static void stop_listener_in_tcpip_thread(void);

typedef struct
{
  uint16_t port;
  int result;
  osSemaphoreId_t done;
} ProgramTcpStartRequest;

typedef struct
{
  osSemaphoreId_t done;
} ProgramTcpStopRequest;

static void close_client_pcb(struct tcp_pcb *pcb)
{
  if (g_flash_programming_active != 0U)
  {
    boot_flash_end_app_storage_programming();
    g_flash_programming_active = 0U;
  }

  if (pcb != 0)
  {
    tcp_arg(pcb, 0);
    tcp_recv(pcb, 0);
    tcp_err(pcb, 0);
    (void)tcp_close(pcb);
  }

  if (g_client_pcb == pcb)
  {
    g_client_pcb = 0;
  }
  g_rx_buffer_len = 0U;
}

static int start_listener_in_tcpip_thread(uint16_t port)
{
  if (g_listen_pcb != 0)
  {
    if (g_listen_port == port)
    {
      (void)printf("ProgramTCP: listener already active on port %u\r\n", (unsigned int)port);
      return 0;
    }

    (void)printf("ProgramTCP: closing stale listener on port %u before opening %u\r\n",
                 (unsigned int)g_listen_port, (unsigned int)port);
    stop_listener_in_tcpip_thread();
  }

  struct tcp_pcb *pcb = tcp_new();
  if (pcb == 0)
  {
    (void)printf("ProgramTCP: tcp_new failed for port %u\r\n", (unsigned int)port);
    return -1;
  }

  err_t bind_result = tcp_bind(pcb, IP_ADDR_ANY, port);
  if (bind_result != ERR_OK)
  {
    (void)printf("ProgramTCP: tcp_bind port %u failed err %d\r\n",
                 (unsigned int)port, (int)bind_result);
    tcp_close(pcb);
    return -1;
  }

  g_listen_pcb = tcp_listen(pcb);
  if (g_listen_pcb == 0)
  {
    (void)printf("ProgramTCP: tcp_listen failed for port %u\r\n", (unsigned int)port);
    tcp_close(pcb);
    return -1;
  }

  g_listen_port = port;
  tcp_accept(g_listen_pcb, program_accept);
  (void)printf("ProgramTCP: listening on port %u pcb=%p\r\n",
               (unsigned int)port, (void *)g_listen_pcb);
  return 0;
}

static void stop_listener_in_tcpip_thread(void)
{
  if (g_client_pcb != 0)
  {
    close_client_pcb(g_client_pcb);
  }

  if (g_listen_pcb != 0)
  {
    tcp_arg(g_listen_pcb, 0);
    tcp_accept(g_listen_pcb, 0);
    (void)tcp_close(g_listen_pcb);
    g_listen_pcb = 0;
    g_listen_port = 0U;
  }
}

static void start_listener_callback(void *argument)
{
  ProgramTcpStartRequest *request = (ProgramTcpStartRequest *)argument;
  request->result = start_listener_in_tcpip_thread(request->port);
  (void)osSemaphoreRelease(request->done);
}

static void stop_listener_callback(void *argument)
{
  ProgramTcpStopRequest *request = (ProgramTcpStopRequest *)argument;
  stop_listener_in_tcpip_thread();
  (void)osSemaphoreRelease(request->done);
}

static void write_u32_le(uint8_t out[4], uint32_t value)
{
  out[0] = (uint8_t)(value & 0xFFU);
  out[1] = (uint8_t)((value >> 8U) & 0xFFU);
  out[2] = (uint8_t)((value >> 16U) & 0xFFU);
  out[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

static void store_u32_setting(uint32_t key, uint32_t value)
{
  uint8_t encoded[4];
  write_u32_le(encoded, value);
  (void)boot_metadata_set(key, encoded, sizeof(encoded));
}

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
  err_t result = tcp_write(pcb, frame, sizeof(frame), TCP_WRITE_FLAG_COPY);
  if (result == ERR_OK)
  {
    result = tcp_output(pcb);
  }
  return result;
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
  err_t result = tcp_write(pcb, frame, sizeof(frame), TCP_WRITE_FLAG_COPY);
  if (result == ERR_OK)
  {
    result = tcp_output(pcb);
  }
  return result;
}

static err_t process_program_frame(struct tcp_pcb *pcb, const ProtoProgFrameHeader *header,
                                   const uint8_t *payload)
{
  if ((header->magic != PROTO_MAGIC) ||
      (header->proto_version != PROTO_VERSION) ||
      (header->seq != g_expected_seq))
  {
    (void)send_nack(pcb, header->seq, -4, 0U);
    return ERR_OK;
  }

  switch ((ProgFrameType)header->frame_type)
  {
    case PROG_FRAME_HELLO:
    {
      if (!resident_security_programming_is_unlocked())
      {
        (void)send_nack(pcb, header->seq, -2, 1U);
        break;
      }

      if (header->payload_len < sizeof(ProgHelloPayload))
      {
        (void)send_nack(pcb, header->seq, -2, 2U);
        break;
      }

      const ProgHelloPayload *hello = (const ProgHelloPayload *)payload;
      if (boot_flash_begin_app_storage_programming() != 0)
      {
        (void)send_nack(pcb, header->seq, -5, 1U);
        break;
      }

      g_flash_programming_active = 1U;
      g_expected_image_size = hello->image_size;
      g_bytes_written = 0U;
      g_program_flags = header->flags;
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
      if ((data->data_len == 0U) || (data->data_len > 1024U) ||
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

      if (g_flash_programming_active != 0U)
      {
        boot_flash_end_app_storage_programming();
        g_flash_programming_active = 0U;
      }

      if ((g_program_flags & PROG_FLAG_RAW_STORAGE) == 0U)
      {
        AppImageHeader image_header;
        if ((boot_image_read_header(&image_header) != BOOT_IMAGE_OK) ||
            (boot_image_validate(&image_header, true) != BOOT_IMAGE_OK))
        {
          (void)printf("ProgramTCP: staged raw payload, skipping app validation\r\n");
        }
        else
        {
          store_u32_setting(BOOT_KV_APP_VALID, 1U);
          store_u32_setting(BOOT_KV_APP_DISABLED, 1U);
          store_u32_setting(BOOT_KV_APP_VERSION, image_header.app_version);
          (void)boot_metadata_save_to_flash();
        }
      }

      (void)send_ack(pcb, header->seq);
      (void)tcp_output(pcb);
      g_stop_after_final_ack = 1U;
      g_expected_seq++;
      break;
    }

    case PROG_FRAME_ABORT:
      if (g_flash_programming_active != 0U)
      {
        boot_flash_end_app_storage_programming();
        g_flash_programming_active = 0U;
      }
      (void)send_ack(pcb, header->seq);
      close_client_pcb(pcb);
      break;

    default:
      (void)send_nack(pcb, header->seq, -2, 0U);
      break;
  }

  return ERR_OK;
}

static err_t append_rx_bytes(const struct pbuf *p)
{
  uint16_t copied = 0U;
  for (const struct pbuf *q = p; q != 0 && copied < p->tot_len; q = q->next)
  {
    if ((uint32_t)g_rx_buffer_len + q->len > sizeof(g_rx_buffer))
    {
      g_rx_buffer_len = 0U;
      return ERR_BUF;
    }

    memcpy(&g_rx_buffer[g_rx_buffer_len], q->payload, q->len);
    g_rx_buffer_len = (uint16_t)(g_rx_buffer_len + q->len);
    copied = (uint16_t)(copied + q->len);
  }

  return ERR_OK;
}

static err_t process_rx_buffer(struct tcp_pcb *pcb)
{
  while (g_rx_buffer_len >= sizeof(ProtoProgFrameHeader))
  {
    ProtoProgFrameHeader header;
    memcpy(&header, g_rx_buffer, sizeof(header));

    const uint16_t frame_len = (uint16_t)(sizeof(ProtoProgFrameHeader) + header.payload_len);
    if (frame_len > sizeof(g_rx_buffer))
    {
      g_rx_buffer_len = 0U;
      (void)send_nack(pcb, header.seq, -3, 0U);
      return ERR_OK;
    }

    if (g_rx_buffer_len < frame_len)
    {
      return ERR_OK;
    }

    (void)process_program_frame(pcb, &header, &g_rx_buffer[sizeof(ProtoProgFrameHeader)]);

    const uint16_t remaining = (uint16_t)(g_rx_buffer_len - frame_len);
    if (remaining != 0U)
    {
      memmove(g_rx_buffer, &g_rx_buffer[frame_len], remaining);
    }
    g_rx_buffer_len = remaining;
  }

  return ERR_OK;
}

static err_t program_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
  (void)arg;

  if ((err != ERR_OK) || (p == 0))
  {
    close_client_pcb(pcb);
    return ERR_OK;
  }

  tcp_recved(pcb, p->tot_len);

  if (append_rx_bytes(p) != ERR_OK)
  {
    (void)send_nack(pcb, g_expected_seq, -3, 1U);
    pbuf_free(p);
    return ERR_OK;
  }

  (void)process_rx_buffer(pcb);
  pbuf_free(p);
  return ERR_OK;
}

static void program_err(void *arg, err_t err)
{
  (void)arg;
  (void)err;
  if (g_flash_programming_active != 0U)
  {
    boot_flash_end_app_storage_programming();
    g_flash_programming_active = 0U;
  }
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
    tcp_abort(newpcb);
    return ERR_ABRT;
  }

  g_client_pcb = newpcb;
  g_expected_seq = 0U;
  g_bytes_written = 0U;
  g_expected_image_size = 0U;
  g_program_flags = 0U;
  g_flash_programming_active = 0U;
  g_rx_buffer_len = 0U;
  tcp_recv(newpcb, program_recv);
  tcp_err(newpcb, program_err);
  return ERR_OK;
}

int proto_program_tcp_start(uint16_t port)
{
  osSemaphoreId_t done = osSemaphoreNew(1U, 0U, &g_start_sem_attr);
  if (done == 0)
  {
    (void)printf("ProgramTCP: start semaphore create failed\r\n");
    return -1;
  }

  ProgramTcpStartRequest request = {
    .port = port,
    .result = -1,
    .done = done,
  };
  if (tcpip_callback(start_listener_callback, &request) != ERR_OK)
  {
    (void)osSemaphoreDelete(done);
    (void)printf("ProgramTCP: start callback queue failed\r\n");
    return -1;
  }

  (void)osSemaphoreAcquire(done, osWaitForever);
  (void)osSemaphoreDelete(done);
  if (request.result != 0)
  {
    (void)printf("ProgramTCP: listener start failed on port %u\r\n", (unsigned int)port);
  }
  return request.result;
}

void proto_program_tcp_stop(void)
{
  osSemaphoreId_t done = osSemaphoreNew(1U, 0U, &g_stop_sem_attr);
  if (done == 0)
  {
    (void)printf("ProgramTCP: stop semaphore create failed\r\n");
    return;
  }

  ProgramTcpStopRequest request = {
    .done = done,
  };
  if (tcpip_callback(stop_listener_callback, &request) == ERR_OK)
  {
    (void)osSemaphoreAcquire(done, osWaitForever);
  }
  (void)osSemaphoreDelete(done);
}

void proto_program_tcp_poll(void)
{
  if (g_stop_after_final_ack != 0U)
  {
    g_stop_after_final_ack = 0U;
    resident_program_manager_mark_stopped();
  }
}
