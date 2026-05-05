#include "resident_network.h"

#include "cmsis_os.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "lwip/udp.h"

#include <string.h>

extern struct netif gnetif;

typedef struct
{
  struct udp_pcb *pcb;
  AppUdpReceiveCallback callback;
  void *context;
} ResidentUdpSocket;

static ResidentUdpSocket g_udp_sockets[4];

static osSemaphoreId_t s_prep_for_reset_sem;
static StaticSemaphore_t s_prep_for_reset_sem_cb;

static void reboot_prep_tcpip_cb(void *ctx)
{
  (void)ctx;
  if (s_prep_for_reset_sem != NULL)
  {
    (void)osSemaphoreRelease(s_prep_for_reset_sem);
  }
}

static void app_udp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                         const ip_addr_t *addr, u16_t port)
{
  (void)pcb;
  ResidentUdpSocket *socket = (ResidentUdpSocket *)arg;
  if ((socket != 0) && (socket->callback != 0) && (p != 0) && IP_IS_V4(addr))
  {
    uint8_t ip[4];
    const ip4_addr_t *ip4 = ip_2_ip4(addr);
    memcpy(ip, &ip4->addr, sizeof(ip));
    socket->callback((const uint8_t *)p->payload, p->len, ip, port, socket->context);
  }

  if (p != 0)
  {
    pbuf_free(p);
  }
}

void resident_network_init(void)
{
  memset(g_udp_sockets, 0, sizeof(g_udp_sockets));

  static const osSemaphoreAttr_t prep_attr = {
    .name = "NetRebootPrep",
    .cb_mem = &s_prep_for_reset_sem_cb,
    .cb_size = sizeof(s_prep_for_reset_sem_cb),
  };
  if (s_prep_for_reset_sem == NULL)
  {
    s_prep_for_reset_sem = osSemaphoreNew(1U, 0U, &prep_attr);
  }
}

int resident_network_udp_open(uint16_t local_port, AppUdpReceiveCallback callback,
                              void *context, AppUdpHandle *handle)
{
  if ((callback == 0) || (handle == 0))
  {
    return -1;
  }

  ResidentUdpSocket *slot = 0;
  for (uint32_t i = 0U; i < (sizeof(g_udp_sockets) / sizeof(g_udp_sockets[0])); i++)
  {
    if (g_udp_sockets[i].pcb == 0)
    {
      slot = &g_udp_sockets[i];
      break;
    }
  }

  if (slot == 0)
  {
    return -1;
  }

  slot->pcb = udp_new();
  if (slot->pcb == 0)
  {
    return -1;
  }

  if (udp_bind(slot->pcb, IP_ADDR_ANY, local_port) != ERR_OK)
  {
    udp_remove(slot->pcb);
    memset(slot, 0, sizeof(*slot));
    return -1;
  }

  slot->callback = callback;
  slot->context = context;
  udp_recv(slot->pcb, app_udp_recv, slot);
  *handle = slot;
  return 0;
}

int resident_network_udp_send(AppUdpHandle handle, const uint8_t remote_ip[4], uint16_t remote_port,
                              const void *payload, size_t length)
{
  ResidentUdpSocket *socket = (ResidentUdpSocket *)handle;
  if ((socket == 0) || (socket->pcb == 0) || (remote_ip == 0) || (payload == 0) || (length > UINT16_MAX))
  {
    return -1;
  }

  struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)length, PBUF_RAM);
  if (p == 0)
  {
    return -1;
  }

  memcpy(p->payload, payload, length);

  ip_addr_t addr;
  IP_ADDR4(&addr, remote_ip[0], remote_ip[1], remote_ip[2], remote_ip[3]);
  err_t result = udp_sendto(socket->pcb, p, &addr, remote_port);
  pbuf_free(p);
  return (result == ERR_OK) ? 0 : -1;
}

void resident_network_udp_close(AppUdpHandle handle)
{
  ResidentUdpSocket *socket = (ResidentUdpSocket *)handle;
  if ((socket != 0) && (socket->pcb != 0))
  {
    udp_remove(socket->pcb);
    memset(socket, 0, sizeof(*socket));
  }
}

int resident_network_get_ipv4(uint8_t ip[4], uint8_t netmask[4], uint8_t gateway[4])
{
  if ((ip == 0) || (netmask == 0) || (gateway == 0))
  {
    return -1;
  }

  memcpy(ip, &ip_2_ip4(&gnetif.ip_addr)->addr, 4U);
  memcpy(netmask, &ip_2_ip4(&gnetif.netmask)->addr, 4U);
  memcpy(gateway, &ip_2_ip4(&gnetif.gw)->addr, 4U);
  return 0;
}

int resident_network_get_mac(uint8_t mac[6])
{
  if (mac == 0)
  {
    return -1;
  }

  memcpy(mac, gnetif.hwaddr, 6U);
  return 0;
}

bool resident_network_link_is_up(void)
{
  return netif_is_link_up(&gnetif);
}

void resident_network_prepare_for_reset(void)
{
  /*
   * ethernetif low_level_output() blocks until HAL_ETH TX-complete, so the reboot EXECUTE
   * reply is already on the wire before the udp/ip path returns. Queue a noop callback so we
   * run once more on the tcpip thread after any prior mailbox work (timers, stray packets).
   */
  if ((s_prep_for_reset_sem == NULL) || (tcpip_callback(reboot_prep_tcpip_cb, NULL) != ERR_OK))
  {
    (void)osDelay(10U);
    return;
  }

  (void)osSemaphoreAcquire(s_prep_for_reset_sem, osWaitForever);
}
