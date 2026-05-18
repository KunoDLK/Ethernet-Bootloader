#include "resident_api.h"

#include "boot_metadata.h"
#include "cmsis_os.h"
#include "resident_device_tree.h"
#include "resident_hardware.h"
#include "resident_network.h"

#include <stdio.h>
#include <string.h>

static AppApi g_app_api;

static AppTaskHandle api_create_task(const char *name, AppTaskEntry entry, void *argument,
                                     uint32_t stack_bytes, uint32_t priority)
{
  if (entry == 0)
  {
    return 0;
  }

  osThreadAttr_t attributes;
  memset(&attributes, 0, sizeof(attributes));
  attributes.name = name;
  attributes.stack_size = stack_bytes;
  attributes.priority = (osPriority_t)priority;
  return osThreadNew((osThreadFunc_t)entry, argument, &attributes);
}

static void api_delete_task(AppTaskHandle task)
{
  osThreadTerminate((osThreadId_t)task);
}

static void api_delay_ms(uint32_t delay_ms)
{
  osDelay(delay_ms);
}

static uint32_t api_uptime_ms(void)
{
  return osKernelGetTickCount();
}

static void api_log_write(const char *message)
{
  if (message != 0)
  {
    (void)printf("App: %s\r\n", message);
  }
}

static int api_storage_read(const char *key, void *data, size_t max_length, size_t *length)
{
  return boot_metadata_storage_read_string_key(key, data, max_length, length);
}

static int api_storage_write(const char *key, const void *data, size_t length)
{
  if (boot_metadata_storage_write_string_key(key, data, length) != 0)
  {
    return -1;
  }
  return boot_metadata_save_to_flash();
}

void resident_api_init(void)
{
  memset(&g_app_api, 0, sizeof(g_app_api));
  g_app_api.version = APP_API_VERSION;

  g_app_api.rtos.create_task = api_create_task;
  g_app_api.rtos.delete_task = api_delete_task;
  g_app_api.rtos.delay_ms = api_delay_ms;
  g_app_api.rtos.uptime_ms = api_uptime_ms;

  g_app_api.net.open_udp = resident_network_udp_open;
  g_app_api.net.send_udp = resident_network_udp_send;
  g_app_api.net.close_udp = resident_network_udp_close;
  g_app_api.net.get_ipv4 = resident_network_get_ipv4;
  g_app_api.net.get_mac = resident_network_get_mac;
  g_app_api.net.link_is_up = resident_network_link_is_up;

  g_app_api.device_tree.mount = resident_device_tree_mount_app;
  g_app_api.device_tree.set_value = resident_device_tree_set_app_value;
  g_app_api.device_tree.register_action = resident_device_tree_register_app_action;
  g_app_api.device_tree.unmount = resident_device_tree_unmount_app;

  g_app_api.hardware.gpio_write = resident_hardware_gpio_write;
  g_app_api.hardware.gpio_read = resident_hardware_gpio_read;
  g_app_api.hardware.digital_input_read = resident_hardware_digital_input_read;
  g_app_api.hardware.adc_read_raw = resident_hardware_adc_read_raw;
  g_app_api.hardware.uart_write = resident_hardware_uart_write;
  g_app_api.hardware.uart_read = resident_hardware_uart_read;
  g_app_api.hardware.led_set_status = resident_hardware_led_set_status;

  g_app_api.storage.read = api_storage_read;
  g_app_api.storage.write = api_storage_write;
  g_app_api.log.write = api_log_write;
}

const AppApi *resident_api_get(void)
{
  return &g_app_api;
}
