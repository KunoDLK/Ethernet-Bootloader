#include "resident_tree_hardware.h"

#include "boot_metadata.h"
#include "resident_tree_core.h"
#include "resident_hardware.h"

#include <stdio.h>
#include <string.h>

static char g_hardware_value_text[6][32];

static void populate_hardware_item(const ResidentTreeNode *node, ResidentTreeListItem *item)
{
  resident_tree_populate_static_list_item(node, item);
}

bool resident_tree_hardware_list_cpu_temp(const ResidentTreeNode *node, const uint8_t *parent_location,
                                          ResidentTreeListItem *item)
{
  (void)parent_location;
  if ((node == 0) || (item == 0))
  {
    return false;
  }
  populate_hardware_item(node, item);
  (void)resident_hardware_get_cpu_temp_text(g_hardware_value_text[0], sizeof(g_hardware_value_text[0]));
  item->value = g_hardware_value_text[0];
  return true;
}

bool resident_tree_hardware_list_poll_period(const ResidentTreeNode *node, const uint8_t *parent_location,
                                             ResidentTreeListItem *item)
{
  (void)parent_location;
  if ((node == 0) || (item == 0))
  {
    return false;
  }
  populate_hardware_item(node, item);
  (void)resident_hardware_get_poll_period_text(g_hardware_value_text[1], sizeof(g_hardware_value_text[1]));
  item->value = g_hardware_value_text[1];
  return true;
}

bool resident_tree_hardware_list_estop(const ResidentTreeNode *node, const uint8_t *parent_location,
                                       ResidentTreeListItem *item)
{
  (void)parent_location;
  if ((node == 0) || (item == 0))
  {
    return false;
  }
  populate_hardware_item(node, item);
  (void)resident_hardware_get_estop_text(g_hardware_value_text[2], sizeof(g_hardware_value_text[2]));
  item->value = g_hardware_value_text[2];
  return true;
}

bool resident_tree_hardware_list_button(const ResidentTreeNode *node, const uint8_t *parent_location,
                                        ResidentTreeListItem *item)
{
  (void)parent_location;
  if ((node == 0) || (item == 0))
  {
    return false;
  }
  populate_hardware_item(node, item);
  (void)resident_hardware_get_button_text(g_hardware_value_text[3], sizeof(g_hardware_value_text[3]));
  item->value = g_hardware_value_text[3];
  return true;
}

bool resident_tree_hardware_list_rail_mode(const ResidentTreeNode *node, const uint8_t *parent_location,
                                           ResidentTreeListItem *item)
{
  if ((node == 0) || (item == 0) || (parent_location == 0))
  {
    return false;
  }
  populate_hardware_item(node, item);
  (void)resident_hardware_get_rail_mode_text((parent_location[1] == 5U) ? RESIDENT_HARDWARE_RAIL_B
                                                                         : RESIDENT_HARDWARE_RAIL_A,
                                             g_hardware_value_text[4], sizeof(g_hardware_value_text[4]));
  item->value = g_hardware_value_text[4];
  return true;
}

bool resident_tree_hardware_list_rail_output(const ResidentTreeNode *node, const uint8_t *parent_location,
                                             ResidentTreeListItem *item)
{
  if ((node == 0) || (item == 0) || (parent_location == 0))
  {
    return false;
  }
  populate_hardware_item(node, item);
  (void)resident_hardware_get_rail_output_text((parent_location[1] == 5U) ? RESIDENT_HARDWARE_RAIL_B
                                                                          : RESIDENT_HARDWARE_RAIL_A,
                                              g_hardware_value_text[5], sizeof(g_hardware_value_text[5]));
  item->value = g_hardware_value_text[5];
  return true;
}
