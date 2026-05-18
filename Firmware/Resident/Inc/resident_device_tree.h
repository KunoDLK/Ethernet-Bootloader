#ifndef RESIDENT_DEVICE_TREE_H
#define RESIDENT_DEVICE_TREE_H

#include "app_api.h"
#include "resident_program_manager.h"

#include <stdbool.h>
#include <stdint.h>

void resident_device_tree_init(void);
int resident_device_tree_mount_app(const char *name, AppDeviceTreeMount *mount);
void resident_device_tree_unmount_app(AppDeviceTreeMount mount);
void resident_device_tree_unmount_all_app(void);
int resident_device_tree_set_app_value(AppDeviceTreeMount mount, const char *path, const char *value);
int resident_device_tree_register_app_action(AppDeviceTreeMount mount, const char *path,
                                             AppDeviceTreeActionCallback callback, void *context);
int resident_device_tree_list(const uint8_t *location, uint8_t depth, uint8_t start_after,
                              uint8_t *response, uint16_t response_max, uint16_t *response_len,
                              bool *has_more);
int resident_device_tree_get(const uint8_t *location, uint8_t depth,
                             uint8_t *response, uint16_t response_max, uint16_t *response_len);
int resident_device_tree_set(const uint8_t *location, uint8_t depth,
                             const uint8_t *value, uint16_t value_len,
                             uint8_t *response, uint16_t response_max, uint16_t *response_len);
int resident_device_tree_execute(const uint8_t *location, uint8_t depth,
                                 const uint8_t *args, uint16_t args_len,
                                 uint8_t *response, uint16_t response_max, uint16_t *response_len);

#endif /* RESIDENT_DEVICE_TREE_H */
