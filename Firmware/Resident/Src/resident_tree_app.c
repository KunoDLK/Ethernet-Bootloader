#include "resident_tree_app.h"

#include "proto_common.h"
#include "resident_tree_core.h"

#include <stdio.h>
#include <string.h>

static bool app_action_name_is_valid(const char *path)
{
  if ((path == 0) || (path[0] == '\0') || (path[0] == '/'))
  {
    return false;
  }

  for (const char *cursor = path; *cursor != '\0'; cursor++)
  {
    if (*cursor == '/')
    {
      return false;
    }
  }

  return true;
}

void resident_tree_app_clear(ResidentTreeAppMount *mount)
{
  if (mount != 0)
  {
    memset(mount, 0, sizeof(*mount));
  }
}

int resident_tree_app_mount_name(ResidentTreeAppMount *mount, const char *name)
{
  if ((mount == 0) || (name == 0) || mount->mounted)
  {
    return -1;
  }

  (void)snprintf(mount->name, sizeof(mount->name), "%s", name);
  mount->mounted = true;
  return 0;
}

void resident_tree_app_unmount(ResidentTreeAppMount *mount)
{
  resident_tree_app_clear(mount);
}

int resident_tree_app_register_action(ResidentTreeAppMount *mount, const char *path,
                                      AppDeviceTreeActionCallback callback, void *context)
{
  if ((mount == 0) || !mount->mounted || !app_action_name_is_valid(path) || (callback == 0))
  {
    return -1;
  }

  for (uint8_t i = 0U; i < mount->action_count; i++)
  {
    if (strncmp(mount->actions[i].path, path, sizeof(mount->actions[i].path)) == 0)
    {
      mount->actions[i].callback = callback;
      mount->actions[i].context = context;
      return 0;
    }
  }

  if (mount->action_count >= RESIDENT_TREE_APP_ACTION_MAX)
  {
    return -1;
  }

  ResidentTreeAppAction *action = &mount->actions[mount->action_count++];
  (void)snprintf(action->path, sizeof(action->path), "%s", path);
  action->callback = callback;
  action->context = context;
  return 0;
}

static void populate_app_item(const ResidentTreeNode *node, ResidentTreeListItem *item)
{
  resident_tree_populate_static_list_item(node, item);
}

bool resident_tree_app_list_root(const ResidentTreeAppMount *mount, const ResidentTreeNode *node,
                                 const uint8_t *parent_location, ResidentTreeListItem *item)
{
  (void)parent_location;
  if ((mount == 0) || (node == 0) || (item == 0) || !mount->mounted)
  {
    return false;
  }

  populate_app_item(node, item);
  item->name = mount->name;
  return true;
}

bool resident_tree_app_list_action(const ResidentTreeAppMount *mount, const ResidentTreeNode *node,
                                   const uint8_t *parent_location, ResidentTreeListItem *item)
{
  (void)parent_location;
  if ((mount == 0) || (node == 0) || (item == 0) || (node->id == 0U) || (node->id > mount->action_count))
  {
    return false;
  }

  populate_app_item(node, item);
  item->name = mount->actions[node->id - 1U].path;
  item->access = 0x22U;
  item->description = "Application action";
  item->action = true;
  return true;
}

int resident_tree_app_execute_action(const ResidentTreeAppMount *mount, const ResidentTreeNode *node,
                                     const uint8_t *location, uint8_t depth, const uint8_t *args,
                                     uint16_t args_len, uint8_t *response, uint16_t response_max,
                                     uint16_t *response_len)
{
  char args_text[128];

  (void)location;
  (void)depth;
  (void)response;
  (void)response_max;

  if ((mount == 0) || (node == 0) || (node->kind != TREE_NODE_APP_ACTION) || !mount->mounted ||
      (node->id == 0U) || (node->id > mount->action_count) || (response_len == 0))
  {
    return PROTO_RESULT_NOT_FOUND;
  }

  const ResidentTreeAppAction *action = &mount->actions[node->id - 1U];
  if (action->callback == 0)
  {
    return PROTO_RESULT_NOT_FOUND;
  }

  if (((args_len != 0U) && (args == 0)) || (args_len >= sizeof(args_text)))
  {
    return PROTO_RESULT_INVALID_VALUE;
  }

  if (args_len != 0U)
  {
    memcpy(args_text, args, args_len);
  }
  args_text[args_len] = '\0';
  *response_len = 0U;

  const int callback_result = action->callback(args_text, action->context);
  return (callback_result == 0) ? PROTO_RESULT_OK : PROTO_RESULT_GENERIC;
}

void resident_tree_app_rebuild_nodes(const ResidentTreeAppMount *mount, ResidentTreeNode *root_node,
                                     ResidentTreeNode *action_nodes)
{
  if ((mount == 0) || (root_node == 0) || (action_nodes == 0))
  {
    return;
  }

  memset(action_nodes, 0, sizeof(ResidentTreeNode) * RESIDENT_TREE_APP_ACTION_MAX);
  root_node->child = 0;

  if (!mount->mounted)
  {
    return;
  }

  for (uint8_t i = 0U; i < mount->action_count; i++)
  {
    action_nodes[i].id = (uint8_t)(i + 1U);
    action_nodes[i].kind = TREE_NODE_APP_ACTION;
    action_nodes[i].child = 0;
    action_nodes[i].next = (i + 1U < mount->action_count) ? &action_nodes[i + 1U] : 0;
    action_nodes[i].execute = 0;
    action_nodes[i].list = 0;
    action_nodes[i].access = 0x22U;
  }

  root_node->child = (mount->action_count != 0U) ? &action_nodes[0] : 0;
}
