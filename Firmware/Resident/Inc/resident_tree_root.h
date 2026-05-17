#ifndef RESIDENT_TREE_ROOT_H
#define RESIDENT_TREE_ROOT_H

#include "resident_tree_app.h"
#include "resident_tree_types.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
  ResidentTreeNode *network;
  ResidentTreeNode *hardware;
  ResidentTreeNode *debug;
  ResidentTreeNode *reboot;
  ResidentTreeNode *app_root;
  ResidentTreeNode *network_mac;
  ResidentTreeNode *ipv4_address;
  ResidentTreeNode *ipv4_subnet;
  ResidentTreeNode *ipv4_gateway;
  ResidentTreeNode *ipv4_dhcp;
  ResidentTreeNode *hardware_cpu_temp;
  ResidentTreeNode *hardware_config;
  ResidentTreeNode *hardware_estop;
  ResidentTreeNode *hardware_rail_a;
  ResidentTreeNode *hardware_rail_b;
  ResidentTreeNode *hardware_button;
  ResidentTreeNode *rail_mode;
  ResidentTreeNode *rail_output;
  ResidentTreeNode *flash_current_settings_slot;
  ResidentTreeNode *flash_bytes_used;
  ResidentTreeNode *flash_bytes_remaining;
  ResidentTreeNode *flash_current_settings_phys_addr;
  ResidentTreeNode *flash_current_settings_object_size;
  ResidentTreeNode *app_action_nodes;
  uint8_t app_action_count;
  ResidentTreeNode *flash_kv_nodes;
  uint16_t flash_kv_count;
  bool app_mounted;
} ResidentTreeRootLayout;

void resident_tree_root_refresh(const ResidentTreeRootLayout *layout);
const ResidentTreeNode *resident_tree_root_first(const ResidentTreeRootLayout *layout);

#endif /* RESIDENT_TREE_ROOT_H */
