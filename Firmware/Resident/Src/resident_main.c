#include "resident_main.h"

#include "boot_app_manager.h"
#include "boot_metadata.h"
#include "cmsis_os.h"
#include "lwip.h"
#include "proto_control_udp.h"
#include "proto_discovery_udp.h"
#include "proto_program_tcp.h"
#include "resident_api.h"
#include "resident_device_tree.h"
#include "resident_hardware.h"
#include "resident_led.h"
#include "resident_network.h"
#include "resident_security.h"

#include <stdio.h>

#define RESIDENT_LINK_WAIT_TIMEOUT_MS 5000U

void resident_main_task(void *argument)
{
  (void)argument;

  (void)resident_led_set_status(APP_LED_STATUS_0, APP_LED_COLOR_RED, APP_LED_MODE_STATIC, 0U);
  boot_metadata_init();
  boot_app_manager_init();
  resident_network_init();
  resident_device_tree_init();
  resident_hardware_init();
  resident_led_init();
  resident_security_init();
  resident_api_init();

  MX_LWIP_Init();

  (void)proto_discovery_udp_start();
  (void)proto_control_udp_start();
  (void)proto_program_tcp_start();
  (void)boot_app_manager_start_if_valid();
  (void)resident_led_set_status(APP_LED_STATUS_0, APP_LED_COLOR_GREEN, APP_LED_MODE_BLINK, 1000U);

  for (;;)
  {
    resident_security_poll();
    proto_program_tcp_poll();
    osDelay(10U);
  }
}
