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

static void write_u32_le(uint8_t out[4], uint32_t value)
{
  out[0] = (uint8_t)(value & 0xFFU);
  out[1] = (uint8_t)((value >> 8U) & 0xFFU);
  out[2] = (uint8_t)((value >> 16U) & 0xFFU);
  out[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

static void resident_apply_default_settings(void)
{
  uint8_t app_status[4];
  uint8_t poll_period[4];
  const uint8_t default_ip[4] = {RESIDENT_IPV4_ADDR0, RESIDENT_IPV4_ADDR1, RESIDENT_IPV4_ADDR2, RESIDENT_IPV4_ADDR3};
  const uint8_t default_mask[4] = {RESIDENT_IPV4_MASK0, RESIDENT_IPV4_MASK1, RESIDENT_IPV4_MASK2, RESIDENT_IPV4_MASK3};
  const uint8_t default_gw[4] = {RESIDENT_IPV4_GW0, RESIDENT_IPV4_GW1, RESIDENT_IPV4_GW2, RESIDENT_IPV4_GW3};
  const uint8_t default_mac[6] = {0x30U, 0x3DU, 0x51U, 0xBAU, 0x00U, 0x00U};
  const uint8_t default_byte = 0U;
  BootMetadataEntryView defaults[] = {
    {BOOT_KV_APP_VALID, {app_status, sizeof(app_status)}},
    {BOOT_KV_APP_DISABLED, {app_status, sizeof(app_status)}},
    {BOOT_KV_APP_VERSION, {app_status, sizeof(app_status)}},
    {BOOT_KV_FAULT_REASON, {app_status, sizeof(app_status)}},
    {BOOT_KV_FAULT_PC, {app_status, sizeof(app_status)}},
    {BOOT_KV_FAULT_LR, {app_status, sizeof(app_status)}},
    {BOOT_KV_IPV4_ADDR, {default_ip, sizeof(default_ip)}},
    {BOOT_KV_IPV4_SUBNET, {default_mask, sizeof(default_mask)}},
    {BOOT_KV_IPV4_GW, {default_gw, sizeof(default_gw)}},
    {BOOT_KV_NET_MAC, {default_mac, sizeof(default_mac)}},
    {BOOT_KV_HW_POLL_MS, {poll_period, sizeof(poll_period)}},
    {BOOT_KV_RAIL_A, {&default_byte, sizeof(default_byte)}},
    {BOOT_KV_RAIL_B, {&default_byte, sizeof(default_byte)}},
    {BOOT_KV_NET_DHCP, {&default_byte, sizeof(default_byte)}},
  };

  write_u32_le(app_status, 0U);
  write_u32_le(poll_period, BOOT_METADATA_DEFAULT_HARDWARE_POLL_PERIOD_MS);

  if (boot_metadata_set_many(defaults, (uint16_t)(sizeof(defaults) / sizeof(defaults[0]))) == 0)
  {
    (void)boot_metadata_save_to_flash();
  }
}

void resident_main_task(void *argument)
{
  (void)argument;

  (void)resident_led_set_status(APP_LED_STATUS_0, APP_LED_COLOR_RED, APP_LED_MODE_STATIC, 0U);
  if (boot_metadata_initialize() != 0)
  {
    resident_apply_default_settings();
  }
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
