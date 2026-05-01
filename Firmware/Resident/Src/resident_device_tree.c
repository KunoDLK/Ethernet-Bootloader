#include "resident_device_tree.h"

#include <stdio.h>
#include <string.h>

typedef struct
{
  char name[32];
  bool mounted;
} ResidentAppTreeMount;

static ResidentAppTreeMount g_app_mount;

void resident_device_tree_init(void)
{
  memset(&g_app_mount, 0, sizeof(g_app_mount));
}

int resident_device_tree_mount_app(const char *name, AppDeviceTreeMount *mount)
{
  if ((name == 0) || (mount == 0) || g_app_mount.mounted)
  {
    return -1;
  }

  (void)snprintf(g_app_mount.name, sizeof(g_app_mount.name), "%s", name);
  g_app_mount.mounted = true;
  *mount = &g_app_mount;
  return 0;
}

void resident_device_tree_unmount_app(AppDeviceTreeMount mount)
{
  if (mount == &g_app_mount)
  {
    memset(&g_app_mount, 0, sizeof(g_app_mount));
  }
}

void resident_device_tree_unmount_all_app(void)
{
  memset(&g_app_mount, 0, sizeof(g_app_mount));
}

int resident_device_tree_set_app_value(AppDeviceTreeMount mount, const char *path, const char *value)
{
  (void)path;
  (void)value;

  return (mount == &g_app_mount) && g_app_mount.mounted ? 0 : -1;
}

int resident_device_tree_register_app_action(AppDeviceTreeMount mount, const char *path,
                                             AppDeviceTreeActionCallback callback, void *context)
{
  (void)path;
  (void)callback;
  (void)context;

  return (mount == &g_app_mount) && g_app_mount.mounted ? 0 : -1;
}
