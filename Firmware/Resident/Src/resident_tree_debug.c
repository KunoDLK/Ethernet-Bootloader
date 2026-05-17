#include "resident_tree_debug.h"

#include "boot_metadata.h"
#include "resident_tree_core.h"
#include "resident_text.h"

#include <stdio.h>
#include <string.h>

static char g_debug_value_text[5][32];
static char g_debug_kv_name[BOOT_METADATA_STORED_KV_MAX][16];
static char g_debug_kv_value[BOOT_METADATA_STORED_KV_MAX][64];

static void populate_debug_item(const ResidentTreeNode *node, ResidentTreeListItem *item)
{
  resident_tree_populate_static_list_item(node, item);
}

static bool debug_stat_value(const ResidentTreeNode *node, ResidentTreeListItem *item, uint8_t stat_index)
{
  populate_debug_item(node, item);
  switch (stat_index)
  {
    case 0U:
      (void)snprintf(g_debug_value_text[0], sizeof(g_debug_value_text[0]), "%lu",
                     (unsigned long)boot_metadata_get_current_settings_slot());
      item->value = g_debug_value_text[0];
      break;
    case 1U:
      (void)snprintf(g_debug_value_text[1], sizeof(g_debug_value_text[1]), "%lu",
                     (unsigned long)boot_metadata_get_flash_bytes_used());
      item->value = g_debug_value_text[1];
      break;
    case 2U:
      (void)snprintf(g_debug_value_text[2], sizeof(g_debug_value_text[2]), "%lu",
                     (unsigned long)boot_metadata_get_flash_bytes_remaining());
      item->value = g_debug_value_text[2];
      break;
    case 3U:
      (void)snprintf(g_debug_value_text[3], sizeof(g_debug_value_text[3]), "0x%08lX",
                     (unsigned long)boot_metadata_get_current_settings_object_phys_addr());
      item->value = g_debug_value_text[3];
      break;
    case 4U:
      (void)snprintf(g_debug_value_text[4], sizeof(g_debug_value_text[4]), "%lu",
                     (unsigned long)boot_metadata_get_current_settings_object_size_bytes());
      item->value = g_debug_value_text[4];
      break;
    default:
      return false;
  }
  return true;
}

bool resident_tree_debug_list_flash_current_settings_slot(const ResidentTreeNode *node,
                                                          const uint8_t *parent_location,
                                                          ResidentTreeListItem *item)
{
  (void)parent_location;
  return (node != 0) && (item != 0) ? debug_stat_value(node, item, 0U) : false;
}

bool resident_tree_debug_list_flash_bytes_used(const ResidentTreeNode *node,
                                               const uint8_t *parent_location,
                                               ResidentTreeListItem *item)
{
  (void)parent_location;
  return (node != 0) && (item != 0) ? debug_stat_value(node, item, 1U) : false;
}

bool resident_tree_debug_list_flash_bytes_remaining(const ResidentTreeNode *node,
                                                    const uint8_t *parent_location,
                                                    ResidentTreeListItem *item)
{
  (void)parent_location;
  return (node != 0) && (item != 0) ? debug_stat_value(node, item, 2U) : false;
}

bool resident_tree_debug_list_flash_current_settings_phys_addr(const ResidentTreeNode *node,
                                                               const uint8_t *parent_location,
                                                               ResidentTreeListItem *item)
{
  (void)parent_location;
  return (node != 0) && (item != 0) ? debug_stat_value(node, item, 3U) : false;
}

bool resident_tree_debug_list_flash_current_settings_object_size(const ResidentTreeNode *node,
                                                                 const uint8_t *parent_location,
                                                                 ResidentTreeListItem *item)
{
  (void)parent_location;
  return (node != 0) && (item != 0) ? debug_stat_value(node, item, 4U) : false;
}

bool resident_tree_debug_list_flash_kv(const ResidentTreeNode *node, const uint8_t *parent_location,
                                       ResidentTreeListItem *item)
{
  uint8_t raw_kv_value[BOOT_METADATA_KV_VALUE_MAX];
  uint32_t raw_key;
  uint16_t raw_len;
  const uint16_t kv_index = (node == 0) ? 0U : (uint16_t)(node->id - 5U);
  const uint16_t buf_index = (uint16_t)(kv_index - 1U);

  (void)parent_location;

  if ((node == 0) || (item == 0) || (kv_index == 0U) ||
      (boot_metadata_read_stored_kv_raw(kv_index, &raw_key, raw_kv_value,
                                        sizeof(raw_kv_value), &raw_len) != 0))
  {
    return false;
  }

  populate_debug_item(node, item);
  (void)snprintf(g_debug_kv_name[buf_index], sizeof(g_debug_kv_name[buf_index]),
                 "0x%08lX", (unsigned long)raw_key);
  resident_text_bytes_to_hex(raw_kv_value, raw_len, g_debug_kv_value[buf_index],
                             sizeof(g_debug_kv_value[buf_index]));
  item->name = g_debug_kv_name[buf_index];
  item->value = g_debug_kv_value[buf_index];
  item->description = "Raw stored boot metadata KV value (hex)";
  return true;
}
