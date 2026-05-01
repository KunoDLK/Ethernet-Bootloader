#include "proto_discovery_udp.h"

#include "boot_metadata.h"
#include "proto_common.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "stm32f4xx_hal.h"

#include <string.h>

extern struct netif gnetif;

typedef struct __attribute__((packed))
{
  ProtoDiscoveryHeader header;
  uint8_t uid[12];
  uint8_t mac[6];
  uint8_t ip[4];
  uint8_t subnet[4];
  uint8_t gateway[4];
  uint32_t capabilities;
  uint32_t resident_version;
  uint32_t app_version;
} DiscoverReply;

static struct udp_pcb *g_discovery_pcb;

static void read_uid(uint8_t uid[12])
{
#ifdef UID_BASE
  memcpy(uid, (const void *)UID_BASE, 12U);
#else
  memset(uid, 0, 12U);
#endif
}

static void discovery_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                           const ip_addr_t *addr, u16_t port)
{
  (void)arg;

  if ((p == 0) || (p->len < sizeof(ProtoDiscoveryHeader)) || !IP_IS_V4(addr))
  {
    if (p != 0)
    {
      pbuf_free(p);
    }
    return;
  }

  const ProtoDiscoveryHeader *request = (const ProtoDiscoveryHeader *)p->payload;
  if ((request->magic != PROTO_MAGIC) ||
      (request->proto_version != PROTO_VERSION) ||
      (request->msg_type != PROTO_MSG_DISCOVER_REQ))
  {
    pbuf_free(p);
    return;
  }

  DiscoverReply reply;
  memset(&reply, 0, sizeof(reply));
  reply.header.magic = PROTO_MAGIC;
  reply.header.proto_version = PROTO_VERSION;
  reply.header.msg_type = PROTO_MSG_DISCOVER_REPLY;
  reply.header.flags = PROTO_FLAG_REPLY;
  reply.header.transaction_id = request->transaction_id;
  read_uid(reply.uid);
  memcpy(reply.mac, gnetif.hwaddr, sizeof(reply.mac));
  memcpy(reply.ip, &ip_2_ip4(&gnetif.ip_addr)->addr, 4U);
  memcpy(reply.subnet, &ip_2_ip4(&gnetif.netmask)->addr, 4U);
  memcpy(reply.gateway, &ip_2_ip4(&gnetif.gw)->addr, 4U);
  reply.capabilities = 0x0FUL;
  reply.resident_version = 1U;
  reply.app_version = boot_metadata_get()->app_version;

  struct pbuf *response = pbuf_alloc(PBUF_TRANSPORT, sizeof(reply), PBUF_RAM);
  if (response != 0)
  {
    const ip_addr_t *reply_addr = addr;
    if (!ip4_addr_netcmp(ip_2_ip4(addr), netif_ip4_addr(&gnetif), netif_ip4_netmask(&gnetif)))
    {
      reply_addr = IP_ADDR_BROADCAST;
    }

    memcpy(response->payload, &reply, sizeof(reply));
    (void)udp_sendto(pcb, response, reply_addr, port);
    pbuf_free(response);
  }

  pbuf_free(p);
}

int proto_discovery_udp_start(void)
{
  g_discovery_pcb = udp_new();
  if (g_discovery_pcb == 0)
  {
    return -1;
  }

  if (udp_bind(g_discovery_pcb, IP_ADDR_ANY, DISCOVERY_PORT) != ERR_OK)
  {
    udp_remove(g_discovery_pcb);
    g_discovery_pcb = 0;
    return -1;
  }

  udp_recv(g_discovery_pcb, discovery_recv, 0);
  return 0;
}
