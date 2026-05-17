#include "resident_tree_network.h"

#include "boot_metadata.h"
#include "resident_tree_core.h"
#include "resident_network.h"
#include "resident_text.h"

#include <stdio.h>
#include <string.h>

#define TREE_ACCESS_READ (0x88U)

static char g_network_value_text[5][32];

static bool metadata_read_bytes(uint32_t key, uint8_t *out, uint16_t expected_len)
{
  BootMetadataValueView value;
  if ((out == 0) || (boot_metadata_get(key, &value) != 0) || (value.value_len != expected_len))
  {
    return false;
  }

  memcpy(out, value.value, expected_len);
  return true;
}

static void metadata_get_ipv4(uint8_t ip[4], uint8_t subnet[4], uint8_t gateway[4])
{
  memset(ip, 0, 4U);
  memset(subnet, 0, 4U);
  memset(gateway, 0, 4U);
  (void)metadata_read_bytes(BOOT_KV_IPV4_ADDR, ip, 4U);
  (void)metadata_read_bytes(BOOT_KV_IPV4_SUBNET, subnet, 4U);
  (void)metadata_read_bytes(BOOT_KV_IPV4_GW, gateway, 4U);
}

static void populate_network_item(const ResidentTreeNode *node, ResidentTreeListItem *item)
{
  resident_tree_populate_static_list_item(node, item);
}

static void format_mac(char *text, size_t text_size)
{
  uint8_t mac[6];
  BootMetadataValueView value;
  if ((boot_metadata_get(BOOT_KV_NET_MAC, &value) == 0) && (value.value_len == sizeof(mac)))
  {
    memcpy(mac, value.value, sizeof(mac));
  }
  else
  {
    memset(mac, 0, sizeof(mac));
  }
  (void)snprintf(text, text_size, "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool network_ipv4_value(const ResidentTreeNode *node, const uint8_t *parent_location,
                               ResidentTreeListItem *item)
{
  uint8_t ip[4];
  uint8_t subnet[4];
  uint8_t gateway[4];
  const uint8_t *selected = ip;

  (void)parent_location;

  if ((node == 0) || (item == 0))
  {
    return false;
  }

  BootMetadataValueView dhcp_value;
  const uint8_t dhcp_on = ((boot_metadata_get(BOOT_KV_NET_DHCP, &dhcp_value) == 0) &&
                           (dhcp_value.value_len == 1U) && (dhcp_value.value[0] != 0U)) ? 1U : 0U;

  if (dhcp_on != 0U)
  {
    if (resident_network_get_ipv4(ip, subnet, gateway) != 0)
    {
      return false;
    }
  }
  else
  {
    metadata_get_ipv4(ip, subnet, gateway);
  }

  if (node->id == 3U)
  {
    selected = subnet;
  }
  else if (node->id == 4U)
  {
    selected = gateway;
  }

  populate_network_item(node, item);
  resident_text_format_ipv4(selected, g_network_value_text[node->id], sizeof(g_network_value_text[node->id]));
  item->value = g_network_value_text[node->id];
  item->access = (dhcp_on != 0U) ? TREE_ACCESS_READ : node->access;
  item->write_effect = (dhcp_on != 0U) ? 0 : node->write_effect;
  item->description = (dhcp_on != 0U) ? "Runtime IPv4 address (DHCP)" : node->description;
  return true;
}

bool resident_tree_network_list_mac(const ResidentTreeNode *node, const uint8_t *parent_location,
                                    ResidentTreeListItem *item)
{
  (void)parent_location;
  if ((node == 0) || (item == 0))
  {
    return false;
  }

  populate_network_item(node, item);
  format_mac(g_network_value_text[1], sizeof(g_network_value_text[1]));
  item->value = g_network_value_text[1];
  return true;
}

bool resident_tree_network_list_dhcp(const ResidentTreeNode *node, const uint8_t *parent_location,
                                     ResidentTreeListItem *item)
{
  (void)parent_location;
  if ((node == 0) || (item == 0))
  {
    return false;
  }

  populate_network_item(node, item);
  BootMetadataValueView value;
  item->value = g_network_value_text[2];
  if ((boot_metadata_get(BOOT_KV_NET_DHCP, &value) == 0) && (value.value_len == 1U) && (value.value[0] != 0U))
  {
    (void)snprintf(g_network_value_text[2], sizeof(g_network_value_text[2]), "%s", "Enabled");
  }
  else
  {
    (void)snprintf(g_network_value_text[2], sizeof(g_network_value_text[2]), "%s", "Disabled");
  }
  return true;
}

bool resident_tree_network_list_ipv4(const ResidentTreeNode *node, const uint8_t *parent_location,
                                     ResidentTreeListItem *item)
{
  return network_ipv4_value(node, parent_location, item);
}
