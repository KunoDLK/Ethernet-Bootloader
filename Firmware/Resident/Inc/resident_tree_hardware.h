#ifndef RESIDENT_TREE_HARDWARE_H
#define RESIDENT_TREE_HARDWARE_H

#include "resident_tree_types.h"

#include <stdbool.h>
#include <stdint.h>

bool resident_tree_hardware_list_cpu_temp(const ResidentTreeNode *node, const uint8_t *parent_location,
                                          ResidentTreeListItem *item);
bool resident_tree_hardware_list_poll_period(const ResidentTreeNode *node, const uint8_t *parent_location,
                                             ResidentTreeListItem *item);
bool resident_tree_hardware_list_estop(const ResidentTreeNode *node, const uint8_t *parent_location,
                                       ResidentTreeListItem *item);
bool resident_tree_hardware_list_button(const ResidentTreeNode *node, const uint8_t *parent_location,
                                        ResidentTreeListItem *item);
bool resident_tree_hardware_list_rail_mode(const ResidentTreeNode *node, const uint8_t *parent_location,
                                           ResidentTreeListItem *item);
bool resident_tree_hardware_list_rail_output(const ResidentTreeNode *node, const uint8_t *parent_location,
                                             ResidentTreeListItem *item);

#endif /* RESIDENT_TREE_HARDWARE_H */
