#include "app_api.h"
#include "app_config.h"

static const AppApi *g_api;
static AppDeviceTreeMount g_tree_mount;

int app_start(const AppApi *api)
{
  if ((api == 0) || (api->version != APP_API_VERSION))
  {
    return -1;
  }

  g_api = api;

  if (g_api->device_tree.mount != 0)
  {
    (void)g_api->device_tree.mount(EXAMPLE_APP_NAME, &g_tree_mount);
    (void)g_api->device_tree.set_value(g_tree_mount, "status", "running");
  }

  if (g_api->log.write != 0)
  {
    g_api->log.write("Example app started");
  }

  if (g_api->hardware.gpio_write != 0)
  {
    (void)g_api->hardware.gpio_write(APP_GPIO_ENABLE_5V_RAIL, true);
  }

  if (g_api->hardware.led_set_status != 0)
  {
    (void)g_api->hardware.led_set_status(APP_LED_STATUS_0, APP_LED_COLOR_BOTH, APP_LED_MODE_BLINK, 500U);
  }

  return 0;
}

void app_stop(void)
{
  if (g_api == 0)
  {
    return;
  }

  if (g_api->hardware.gpio_write != 0)
  {
    (void)g_api->hardware.gpio_write(APP_GPIO_ENABLE_5V_RAIL, false);
  }

  if (g_api->hardware.led_set_status != 0)
  {
    (void)g_api->hardware.led_set_status(APP_LED_STATUS_0, APP_LED_COLOR_GREEN, APP_LED_MODE_STATIC, 0U);
  }

  if (g_api->device_tree.unmount != 0)
  {
    g_api->device_tree.unmount(g_tree_mount);
  }

  if (g_api->log.write != 0)
  {
    g_api->log.write("Example app stopped");
  }

  g_tree_mount = 0;
  g_api = 0;
}
