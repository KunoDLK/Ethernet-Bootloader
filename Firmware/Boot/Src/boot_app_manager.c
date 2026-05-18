#include "boot_app_manager.h"

#include "app_api.h"
#include "app_image.h"
#include "boot_image.h"
#include "boot_metadata.h"
#include "cmsis_os.h"
#include "resident_api.h"
#include "resident_device_tree.h"

static bool g_app_running;
static bool g_app_paused;
static AppStopFn g_app_stop;
static AppEntryPoint g_app_entry;
static StaticTask_t g_app_task_cb;
static StackType_t g_app_task_stack[512];
static osThreadId_t g_app_task;

static void app_task(void *argument)
{
  (void)argument;

  const AppApi *api = resident_api_get();
  if ((g_app_entry == 0) || (api == 0) || (g_app_entry(api) != 0))
  {
    g_app_running = false;
    g_app_paused = false;
    g_app_task = 0;
    g_app_stop = 0;
    g_app_entry = 0;
    osThreadExit();
  }

  for (;;)
  {
    osDelay(1000U);
  }
}

static uint32_t read_u32_le(const uint8_t *value)
{
  return (uint32_t)value[0] | ((uint32_t)value[1] << 8U) |
         ((uint32_t)value[2] << 16U) | ((uint32_t)value[3] << 24U);
}

static bool metadata_u32_is_nonzero(uint32_t key)
{
  BootMetadataValueView value;
  if ((boot_metadata_get(key, &value) != 0) || (value.value_len != sizeof(uint32_t)))
  {
    return false;
  }
  return read_u32_le(value.value) != 0U;
}

void boot_app_manager_init(void)
{
  g_app_running = false;
  g_app_paused = false;
  g_app_stop = 0;
  g_app_entry = 0;
  g_app_task = 0;
}

static int start_loaded_app(bool require_metadata_valid)
{
  if (require_metadata_valid &&
      (metadata_u32_is_nonzero(BOOT_KV_APP_DISABLED) || !metadata_u32_is_nonzero(BOOT_KV_APP_VALID)))
  {
    return -1;
  }

  if (g_app_running)
  {
    return 0;
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

  g_app_entry = (AppEntryPoint)boot_image_entry_address(&header);
  if (g_app_entry == 0)
  {
    return -1;
  }

  g_app_stop = (AppStopFn)boot_image_stop_address(&header);

  const osThreadAttr_t task_attr = {
    .name = "LoadedApp",
    .cb_mem = &g_app_task_cb,
    .cb_size = sizeof(g_app_task_cb),
    .stack_mem = g_app_task_stack,
    .stack_size = sizeof(g_app_task_stack),
    .priority = osPriorityNormal,
  };

  g_app_task = osThreadNew(app_task, 0, &task_attr);
  if (g_app_task == 0)
  {
    g_app_stop = 0;
    g_app_entry = 0;
    return -1;
  }
  g_app_running = true;
  g_app_paused = false;
  return 0;
}

int boot_app_manager_start_if_valid(void)
{
  return start_loaded_app(true);
}

int boot_app_manager_start_force(void)
{
  return start_loaded_app(false);
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
  if (g_app_task != 0)
  {
    (void)osThreadTerminate(g_app_task);
  }
  g_app_running = false;
  g_app_paused = false;
  g_app_stop = 0;
  g_app_entry = 0;
  g_app_task = 0;
  return 0;
}

int boot_app_manager_pause(void)
{
  if (!g_app_running || (g_app_task == 0))
  {
    return -1;
  }
  if (g_app_paused)
  {
    return 0;
  }
  if (osThreadSuspend(g_app_task) != osOK)
  {
    return -1;
  }
  g_app_paused = true;
  return 0;
}

int boot_app_manager_resume(void)
{
  if (!g_app_running || (g_app_task == 0))
  {
    return boot_app_manager_start_if_valid();
  }
  if (!g_app_paused)
  {
    return 0;
  }
  if (osThreadResume(g_app_task) != osOK)
  {
    return -1;
  }
  g_app_paused = false;
  return 0;
}

bool boot_app_manager_is_running(void)
{
  return g_app_running;
}

bool boot_app_manager_is_paused(void)
{
  return g_app_paused;
}
