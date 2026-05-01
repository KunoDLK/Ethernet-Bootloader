#ifndef RESIDENT_DEVICE_TREE_H
#define RESIDENT_DEVICE_TREE_H

#include "app_api.h"

void resident_device_tree_init(void);
int resident_device_tree_mount_app(const char *name, AppDeviceTreeMount *mount);
void resident_device_tree_unmount_app(AppDeviceTreeMount mount);
void resident_device_tree_unmount_all_app(void);
int resident_device_tree_set_app_value(AppDeviceTreeMount mount, const char *path, const char *value);
int resident_device_tree_register_app_action(AppDeviceTreeMount mount, const char *path,
                                             AppDeviceTreeActionCallback callback, void *context);

#endif /* RESIDENT_DEVICE_TREE_H */
