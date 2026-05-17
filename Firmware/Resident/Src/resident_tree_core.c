#include "resident_tree_core.h"

#include <stdio.h>
#include <string.h>

const ResidentTreeNode *resident_tree_find_child_by_id(const ResidentTreeNode *first, uint8_t id)
{
  for (const ResidentTreeNode *node = first; node != 0; node = node->next)
  {
    if (node->id == id)
    {
      return node;
    }
  }
  return 0;
}

const ResidentTreeNode *resident_tree_resolve_location_node(const ResidentTreeNode *root,
                                                            const uint8_t *location, uint8_t depth)
{
  if ((root == 0) || (location == 0) || (depth == 0U))
  {
    return 0;
  }

  const ResidentTreeNode *siblings = root;
  const ResidentTreeNode *node = 0;
  for (uint8_t level = 0U; level < depth; level++)
  {
    node = resident_tree_find_child_by_id(siblings, location[level]);
    if (node == 0)
    {
      return 0;
    }
    siblings = node->child;
  }
  return node;
}

void resident_tree_populate_static_list_item(const ResidentTreeNode *node, ResidentTreeListItem *item)
{
  item->id = node->id;
  item->has_children = (node->child != 0) ? 1U : 0U;
  item->access = node->access;
  item->name = node->name;
  item->value = "";
  item->write_effect = node->write_effect;
  item->description = node->description;
  item->action = node->action;
  item->unit = node->unit;
  item->enum_values = node->enum_values;
}

int resident_tree_append_u16(uint8_t *response, uint16_t response_max, uint16_t *offset, uint16_t value)
{
  if ((*offset + sizeof(value)) > response_max)
  {
    return -1;
  }

  memcpy(response + *offset, &value, sizeof(value));
  *offset += sizeof(value);
  return 0;
}

int resident_tree_append_bytes(uint8_t *response, uint16_t response_max, uint16_t *offset,
                              const void *data, uint16_t data_len)
{
  if ((*offset + data_len) > response_max)
  {
    return -1;
  }

  if (data_len != 0U)
  {
    memcpy(response + *offset, data, data_len);
    *offset += data_len;
  }

  return 0;
}

int resident_tree_append_list_item(uint8_t *response, uint16_t response_max, uint16_t *offset,
                                  const ResidentTreeListItem *item)
{
  char json[256];
  int written = snprintf(json, sizeof(json), "{\"Name\":\"%s\",\"Value\":\"%s\"",
                         item->name, item->value);
  if ((written < 0) || ((size_t)written >= sizeof(json)))
  {
    return -1;
  }

  size_t json_len = (size_t)written;
  if (item->write_effect != 0)
  {
    written = snprintf(json + json_len, sizeof(json) - json_len,
                       ",\"WriteEffect\":\"%s\"", item->write_effect);
    if ((written < 0) || ((json_len + (size_t)written) >= sizeof(json)))
    {
      return -1;
    }
    json_len += (size_t)written;
  }

  if (item->description != 0)
  {
    written = snprintf(json + json_len, sizeof(json) - json_len,
                       ",\"Description\":\"%s\"", item->description);
    if ((written < 0) || ((json_len + (size_t)written) >= sizeof(json)))
    {
      return -1;
    }
    json_len += (size_t)written;
  }

  if (item->action)
  {
    written = snprintf(json + json_len, sizeof(json) - json_len, ",\"Action\":true");
    if ((written < 0) || ((json_len + (size_t)written) >= sizeof(json)))
    {
      return -1;
    }
    json_len += (size_t)written;
  }

  if (item->unit != 0)
  {
    written = snprintf(json + json_len, sizeof(json) - json_len,
                       ",\"Unit\":\"%s\"", item->unit);
    if ((written < 0) || ((json_len + (size_t)written) >= sizeof(json)))
    {
      return -1;
    }
    json_len += (size_t)written;
  }

  if (item->enum_values != 0)
  {
    written = snprintf(json + json_len, sizeof(json) - json_len,
                       ",\"Enum\":[%s]", item->enum_values);
    if ((written < 0) || ((json_len + (size_t)written) >= sizeof(json)))
    {
      return -1;
    }
    json_len += (size_t)written;
  }

  written = snprintf(json + json_len, sizeof(json) - json_len, "}");
  if ((written < 0) || ((json_len + (size_t)written) >= sizeof(json)))
  {
    return -1;
  }
  json_len += (size_t)written;

  if ((*offset + 5U + (uint16_t)json_len) > response_max)
  {
    return -1;
  }

  response[(*offset)++] = item->id;
  response[(*offset)++] = item->has_children;
  response[(*offset)++] = item->access;
  if (resident_tree_append_u16(response, response_max, offset, (uint16_t)json_len) != 0)
  {
    return -1;
  }
  return resident_tree_append_bytes(response, response_max, offset, json, (uint16_t)json_len);
}

uint16_t resident_tree_list_item_encoded_len(const ResidentTreeListItem *item)
{
  uint8_t scratch[512];
  uint16_t offset = 0U;

  if (resident_tree_append_list_item(scratch, sizeof(scratch), &offset, item) != 0)
  {
    return 0U;
  }

  return offset;
}
