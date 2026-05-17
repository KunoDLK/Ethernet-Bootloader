#include "resident_tree_root.h"

#include <string.h>

#define TREE_ACCESS_READ                (0x88U)
#define TREE_ACCESS_EXECUTE             (0x22U)

static void link_static_nodes(const ResidentTreeRootLayout *layout)
{
  if (layout == 0)
  {
    return;
  }

  if ((layout->network != 0) && (layout->hardware != 0))
  {
    layout->network->next = layout->hardware;
  }
  if ((layout->hardware != 0) && (layout->debug != 0))
  {
    layout->hardware->next = layout->debug;
  }
  if ((layout->debug != 0) && (layout->reboot != 0))
  {
    layout->debug->next = layout->reboot;
  }
  if (layout->reboot != 0)
  {
    layout->reboot->next = (layout->app_mounted && (layout->app_root != 0)) ? layout->app_root : 0;
  }

  if ((layout->network_mac != 0) && (layout->ipv4_dhcp != 0))
  {
    layout->network_mac->next = layout->ipv4_dhcp;
  }
  if ((layout->ipv4_dhcp != 0) && (layout->ipv4_address != 0))
  {
    layout->ipv4_dhcp->next = layout->ipv4_address;
  }
  if ((layout->ipv4_address != 0) && (layout->ipv4_subnet != 0))
  {
    layout->ipv4_address->next = layout->ipv4_subnet;
  }
  if ((layout->ipv4_subnet != 0) && (layout->ipv4_gateway != 0))
  {
    layout->ipv4_subnet->next = layout->ipv4_gateway;
  }
  if (layout->ipv4_gateway != 0)
  {
    layout->ipv4_gateway->next = 0;
  }

  if ((layout->hardware_cpu_temp != 0) && (layout->hardware_config != 0))
  {
    layout->hardware_cpu_temp->next = layout->hardware_config;
  }
  if ((layout->hardware_config != 0) && (layout->hardware_estop != 0))
  {
    layout->hardware_config->next = layout->hardware_estop;
  }
  if ((layout->hardware_estop != 0) && (layout->hardware_rail_a != 0))
  {
    layout->hardware_estop->next = layout->hardware_rail_a;
  }
  if ((layout->hardware_rail_a != 0) && (layout->hardware_rail_b != 0))
  {
    layout->hardware_rail_a->next = layout->hardware_rail_b;
  }
  if ((layout->hardware_rail_b != 0) && (layout->hardware_button != 0))
  {
    layout->hardware_rail_b->next = layout->hardware_button;
  }
  if (layout->hardware_button != 0)
  {
    layout->hardware_button->next = 0;
  }
  if ((layout->rail_mode != 0) && (layout->rail_output != 0))
  {
    layout->rail_mode->next = layout->rail_output;
  }
  if (layout->rail_output != 0)
  {
    layout->rail_output->next = 0;
  }

  if ((layout->flash_current_settings_slot != 0) && (layout->flash_bytes_used != 0))
  {
    layout->flash_current_settings_slot->next = layout->flash_bytes_used;
  }
  if ((layout->flash_bytes_used != 0) && (layout->flash_bytes_remaining != 0))
  {
    layout->flash_bytes_used->next = layout->flash_bytes_remaining;
  }
  if ((layout->flash_bytes_remaining != 0) && (layout->flash_current_settings_phys_addr != 0))
  {
    layout->flash_bytes_remaining->next = layout->flash_current_settings_phys_addr;
  }
  if ((layout->flash_current_settings_phys_addr != 0) &&
      (layout->flash_current_settings_object_size != 0))
  {
    layout->flash_current_settings_phys_addr->next = layout->flash_current_settings_object_size;
  }
  if (layout->flash_current_settings_object_size != 0)
  {
    layout->flash_current_settings_object_size->next = 0;
  }

  if (layout->app_root != 0)
  {
    layout->app_root->next = 0;
    layout->app_root->child = (layout->app_mounted && (layout->app_action_count != 0U))
                                ? layout->app_action_nodes : 0;
  }
}

static void link_dynamic_app_nodes(const ResidentTreeRootLayout *layout)
{
  if ((layout == 0) || (layout->app_action_nodes == 0))
  {
    return;
  }

  memset(layout->app_action_nodes, 0, sizeof(ResidentTreeNode) * layout->app_action_count);
  for (uint8_t i = 0U; i < layout->app_action_count; i++)
  {
    layout->app_action_nodes[i].id = (uint8_t)(i + 1U);
    layout->app_action_nodes[i].kind = TREE_NODE_APP_ACTION;
    layout->app_action_nodes[i].next = (i + 1U < layout->app_action_count)
                                         ? &layout->app_action_nodes[i + 1U] : 0;
    layout->app_action_nodes[i].child = 0;
    layout->app_action_nodes[i].access = TREE_ACCESS_EXECUTE;
  }
}

static void link_dynamic_flash_kv_nodes(const ResidentTreeRootLayout *layout)
{
  if ((layout == 0) || (layout->flash_kv_nodes == 0))
  {
    return;
  }

  memset(layout->flash_kv_nodes, 0, sizeof(ResidentTreeNode) * layout->flash_kv_count);
  if (layout->flash_current_settings_object_size != 0)
  {
    layout->flash_current_settings_object_size->next = (layout->flash_kv_count != 0U)
                                                         ? layout->flash_kv_nodes : 0;
  }
  for (uint16_t i = 0U; i < layout->flash_kv_count; i++)
  {
    const uint8_t base_id = (layout->flash_current_settings_object_size != 0)
                              ? layout->flash_current_settings_object_size->id : 0U;
    layout->flash_kv_nodes[i].id = (uint8_t)(base_id + i + 1U);
    layout->flash_kv_nodes[i].kind = TREE_NODE_FLASH_KV;
    layout->flash_kv_nodes[i].next = (i + 1U < layout->flash_kv_count) ? &layout->flash_kv_nodes[i + 1U] : 0;
    layout->flash_kv_nodes[i].child = 0;
    layout->flash_kv_nodes[i].access = TREE_ACCESS_READ;
  }
}

void resident_tree_root_refresh(const ResidentTreeRootLayout *layout)
{
  if (layout == 0)
  {
    return;
  }

  link_static_nodes(layout);
  link_dynamic_app_nodes(layout);
  link_dynamic_flash_kv_nodes(layout);
}

const ResidentTreeNode *resident_tree_root_first(const ResidentTreeRootLayout *layout)
{
  return (layout == 0) ? 0 : layout->network;
}
