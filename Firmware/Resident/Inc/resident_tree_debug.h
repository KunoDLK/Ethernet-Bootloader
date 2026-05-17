#ifndef RESIDENT_TREE_DEBUG_H
#define RESIDENT_TREE_DEBUG_H

#include "resident_tree_types.h"

#include <stdbool.h>
#include <stdint.h>

bool resident_tree_debug_list_flash_current_settings_slot(const ResidentTreeNode *node,
                                                          const uint8_t *parent_location,
                                                          ResidentTreeListItem *item);
bool resident_tree_debug_list_flash_bytes_used(const ResidentTreeNode *node,
                                               const uint8_t *parent_location,
                                               ResidentTreeListItem *item);
bool resident_tree_debug_list_flash_bytes_remaining(const ResidentTreeNode *node,
                                                    const uint8_t *parent_location,
                                                    ResidentTreeListItem *item);
bool resident_tree_debug_list_flash_current_settings_phys_addr(const ResidentTreeNode *node,
                                                               const uint8_t *parent_location,
                                                               ResidentTreeListItem *item);
bool resident_tree_debug_list_flash_current_settings_object_size(const ResidentTreeNode *node,
                                                                 const uint8_t *parent_location,
                                                                 ResidentTreeListItem *item);
bool resident_tree_debug_list_flash_kv(const ResidentTreeNode *node, const uint8_t *parent_location,
                                       ResidentTreeListItem *item);

#endif /* RESIDENT_TREE_DEBUG_H */
