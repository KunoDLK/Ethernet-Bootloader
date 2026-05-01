#include "boot_app_manager.h"

#include "app_api.h"
#include "app_image.h"
#include "boot_image.h"
#include "boot_metadata.h"
#include "resident_api.h"
#include "resident_device_tree.h"

static bool g_app_running;
static AppStopFn g_app_stop;

void boot_app_manager_init(void)
{
  g_app_running = false;
  g_app_stop = 0;
}

int boot_app_manager_start_if_valid(void)
{
  if (!boot_metadata_app_is_enabled() || !boot_metadata_app_is_valid())
  {
    return -1;
  }

  AppImageHeader header;
  if (boot_image_read_header(&header) != BOOT_IMAGE_OK)
  {
    return -1;
  }

  if (boot_image_load_to_ccmram(&header) != BOOT_IMAGE_OK)
  {
    return -1;
  }

  AppEntryPoint entry = (AppEntryPoint)boot_image_entry_address(&header);
  if (entry == 0)
  {
    return -1;
  }

  g_app_stop = (AppStopFn)boot_image_stop_address(&header);

  const AppApi *api = resident_api_get();
  if (entry(api) != 0)
  {
    g_app_stop = 0;
    return -1;
  }

  g_app_running = true;
  return 0;
}

int boot_app_manager_stop(void)
{
  if (!g_app_running)
  {
    return 0;
  }

  if (g_app_stop != 0)
  {
    g_app_stop();
  }

  resident_device_tree_unmount_all_app();
  g_app_running = false;
  g_app_stop = 0;
  return 0;
}

bool boot_app_manager_is_running(void)
{
  return g_app_running;
}
