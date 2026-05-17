#ifndef RESIDENT_TREE_CORE_H
#define RESIDENT_TREE_CORE_H

#include "resident_tree_types.h"

#include <stdbool.h>
#include <stdint.h>

const ResidentTreeNode *resident_tree_find_child_by_id(const ResidentTreeNode *first, uint8_t id);
const ResidentTreeNode *resident_tree_resolve_location_node(const ResidentTreeNode *root,
                                                            const uint8_t *location, uint8_t depth);
void resident_tree_populate_static_list_item(const ResidentTreeNode *node, ResidentTreeListItem *item);
int resident_tree_append_u16(uint8_t *response, uint16_t response_max, uint16_t *offset, uint16_t value);
int resident_tree_append_bytes(uint8_t *response, uint16_t response_max, uint16_t *offset,
                              const void *data, uint16_t data_len);
int resident_tree_append_list_item(uint8_t *response, uint16_t response_max, uint16_t *offset,
                                  const ResidentTreeListItem *item);
uint16_t resident_tree_list_item_encoded_len(const ResidentTreeListItem *item);

#endif /* RESIDENT_TREE_CORE_H */
