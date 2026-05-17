#ifndef RESIDENT_TREE_NETWORK_H
#define RESIDENT_TREE_NETWORK_H

#include "resident_tree_types.h"

#include <stdbool.h>
#include <stdint.h>

bool resident_tree_network_list_mac(const ResidentTreeNode *node, const uint8_t *parent_location,
                                    ResidentTreeListItem *item);
bool resident_tree_network_list_dhcp(const ResidentTreeNode *node, const uint8_t *parent_location,
                                     ResidentTreeListItem *item);
bool resident_tree_network_list_ipv4(const ResidentTreeNode *node, const uint8_t *parent_location,
                                     ResidentTreeListItem *item);

#endif /* RESIDENT_TREE_NETWORK_H */
