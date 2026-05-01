#ifndef RESIDENT_NETWORK_H
#define RESIDENT_NETWORK_H

#include "app_api.h"

void resident_network_init(void);
int resident_network_udp_open(uint16_t local_port, AppUdpReceiveCallback callback,
                              void *context, AppUdpHandle *handle);
int resident_network_udp_send(AppUdpHandle handle, const uint8_t remote_ip[4], uint16_t remote_port,
                              const void *payload, size_t length);
void resident_network_udp_close(AppUdpHandle handle);
int resident_network_get_ipv4(uint8_t ip[4], uint8_t netmask[4], uint8_t gateway[4]);
int resident_network_get_mac(uint8_t mac[6]);
bool resident_network_link_is_up(void);

#endif /* RESIDENT_NETWORK_H */
