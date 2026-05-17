#ifndef RESIDENT_TREE_APP_H
#define RESIDENT_TREE_APP_H

#include "app_api.h"
#include "resident_tree_types.h"

#include <stdbool.h>
#include <stdint.h>

#define RESIDENT_TREE_APP_ACTION_MAX (8U)

typedef struct
{
  char path[32];
  AppDeviceTreeActionCallback callback;
  void *context;
} ResidentTreeAppAction;

typedef struct
{
  char name[32];
  ResidentTreeAppAction actions[RESIDENT_TREE_APP_ACTION_MAX];
  uint8_t action_count;
  bool mounted;
} ResidentTreeAppMount;

void resident_tree_app_clear(ResidentTreeAppMount *mount);
int resident_tree_app_mount_name(ResidentTreeAppMount *mount, const char *name);
void resident_tree_app_unmount(ResidentTreeAppMount *mount);
int resident_tree_app_register_action(ResidentTreeAppMount *mount, const char *path,
                                      AppDeviceTreeActionCallback callback, void *context);
bool resident_tree_app_list_root(const ResidentTreeAppMount *mount, const ResidentTreeNode *node,
                                 const uint8_t *parent_location, ResidentTreeListItem *item);
bool resident_tree_app_list_action(const ResidentTreeAppMount *mount, const ResidentTreeNode *node,
                                   const uint8_t *parent_location, ResidentTreeListItem *item);
int resident_tree_app_execute_action(const ResidentTreeAppMount *mount, const ResidentTreeNode *node,
                                     const uint8_t *location, uint8_t depth, const uint8_t *args,
                                     uint16_t args_len, uint8_t *response, uint16_t response_max,
                                     uint16_t *response_len);
void resident_tree_app_rebuild_nodes(const ResidentTreeAppMount *mount, ResidentTreeNode *root_node,
                                     ResidentTreeNode *action_nodes);

#endif /* RESIDENT_TREE_APP_H */
