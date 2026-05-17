#ifndef RESIDENT_TREE_TYPES_H
#define RESIDENT_TREE_TYPES_H

#include "resident_hardware.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
  uint8_t id;
  uint8_t has_children;
  uint8_t access;
  const char *name;
  const char *value;
  const char *write_effect;
  const char *description;
  bool action;
  const char *unit;
  const char *enum_values;
} ResidentTreeListItem;

typedef enum
{
  TREE_NODE_STATIC = 0,
  TREE_NODE_APP_ACTION,
  TREE_NODE_FLASH_KV,
} ResidentTreeNodeKind;

typedef struct ResidentTreeNode ResidentTreeNode;

typedef int (*ResidentTreeExecuteCallback)(const ResidentTreeNode *node, const uint8_t *location,
                                           uint8_t depth, const uint8_t *args, uint16_t args_len,
                                           uint8_t *response, uint16_t response_max,
                                           uint16_t *response_len);

typedef bool (*ResidentTreeListCallback)(const ResidentTreeNode *node, const uint8_t *parent_location,
                                         ResidentTreeListItem *item);

struct ResidentTreeNode
{
  uint8_t id;
  ResidentTreeNodeKind kind;
  const ResidentTreeNode *next;
  const ResidentTreeNode *child;
  ResidentTreeExecuteCallback execute;
  ResidentTreeListCallback list;
  const char *name;
  uint8_t access;
  const char *write_effect;
  const char *description;
  bool action;
  const char *unit;
  const char *enum_values;
};

#endif /* RESIDENT_TREE_TYPES_H */
