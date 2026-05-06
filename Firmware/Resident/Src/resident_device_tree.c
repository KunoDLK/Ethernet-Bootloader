#include "resident_device_tree.h"

#include "boot_metadata.h"
#include "cmsis_os.h"
#include "lwip/netif.h"
#include "proto_common.h"
#include "resident_hardware.h"
#include "resident_network.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

extern struct netif gnetif;

#define TREE_ID_NETWORK                 (1U)
#define TREE_ID_REBOOT                  (2U)
#define TREE_ID_APP                     (3U)
#define TREE_ID_HARDWARE                (4U)
#define TREE_ID_DEBUG                   (5U)
#define TREE_ID_NETWORK_MAC             (1U)
#define TREE_ID_IPV4_ADDRESS            (2U)
#define TREE_ID_IPV4_SUBNET             (3U)
#define TREE_ID_IPV4_GATEWAY            (4U)
#define TREE_ID_IPV4_DHCP               (5U)
#define TREE_ID_HARDWARE_CPU_TEMP       (1U)
#define TREE_ID_HARDWARE_CONFIG         (2U)
#define TREE_ID_HARDWARE_ESTOP          (3U)
#define TREE_ID_HARDWARE_RAIL_A         (4U)
#define TREE_ID_HARDWARE_RAIL_B         (5U)
#define TREE_ID_HARDWARE_BUTTON         (6U)
#define TREE_ID_HARDWARE_POLL_PERIOD    (1U)
#define TREE_ID_HARDWARE_RAIL_MODE      (1U)
#define TREE_ID_HARDWARE_RAIL_OUTPUT    (2U)

#define TREE_ID_DEBUG_FLASH                      (1U)
#define TREE_ID_FLASH_CURRENT_SETTINGS_SLOT          (1U)
#define TREE_ID_FLASH_BYTES_USED                     (2U)
#define TREE_ID_FLASH_BYTES_REMAINING                (3U)
#define TREE_ID_FLASH_CURRENT_SETTINGS_PHYS_ADDR    (4U)
#define TREE_ID_FLASH_CURRENT_SETTINGS_OBJECT_SIZE  (5U)

#define TREE_ACCESS_READ                (0x88U)
#define TREE_ACCESS_READ_WRITE          (0xCCU)
#define TREE_ACCESS_EXECUTE             (0x22U)
#define TREE_ACCESS_READ_EXECUTE        (0xAAU)
#define TREE_WRITE_EFFECT_REBOOT        "reboot_required"
#define TREE_APP_ACTION_MAX             (8U)
#define TREE_RAIL_MODE_ENUM_JSON        "\"Enabled\",\"Disabled\",\"Follow Estop\""
#define TREE_NET_DHCP_ENUM_JSON         "\"Disabled\",\"Enabled\""

typedef struct
{
  char path[32];
  AppDeviceTreeActionCallback callback;
  void *context;
} ResidentAppTreeAction;

typedef struct
{
  char name[32];
  ResidentAppTreeAction actions[TREE_APP_ACTION_MAX];
  uint8_t action_count;
  bool mounted;
} ResidentAppTreeMount;

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

static ResidentAppTreeMount g_app_mount;
static volatile bool g_reboot_requested;
static StaticTask_t g_reboot_task_cb;
static StackType_t g_reboot_task_stack[128];

static void format_ipv4(const uint8_t value[4], char *text, size_t text_size)
{
  (void)snprintf(text, text_size, "%u.%u.%u.%u", value[0], value[1], value[2], value[3]);
}

static void format_mac(char *text, size_t text_size)
{
  uint8_t mac[6];
  boot_metadata_get_mac(mac);
  (void)snprintf(text, text_size, "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int hex_nibble(uint8_t ch)
{
  if ((ch >= (uint8_t)'0') && (ch <= (uint8_t)'9'))
  {
    return (int)(ch - (uint8_t)'0');
  }

  if ((ch >= (uint8_t)'A') && (ch <= (uint8_t)'F'))
  {
    return (int)(ch - (uint8_t)'A') + 10;
  }

  if ((ch >= (uint8_t)'a') && (ch <= (uint8_t)'f'))
  {
    return (int)(ch - (uint8_t)'a') + 10;
  }

  return -1;
}

static bool parse_mac_text(const uint8_t *text, uint16_t text_len, uint8_t value[6])
{
  bool any_nonzero = false;

  if ((text == 0) || (text_len != 17U) || (value == 0))
  {
    return false;
  }

  for (uint8_t i = 0U; i < 6U; i++)
  {
    const uint16_t offset = (uint16_t)i * 3U;
    const int high = hex_nibble(text[offset]);
    const int low = hex_nibble(text[offset + 1U]);
    if ((high < 0) || (low < 0))
    {
      return false;
    }

    value[i] = (uint8_t)(((uint8_t)high << 4U) | (uint8_t)low);
    any_nonzero = any_nonzero || (value[i] != 0U);

    if ((i < 5U) && (text[offset + 2U] != (uint8_t)':'))
    {
      return false;
    }
  }

  return any_nonzero;
}

static bool parse_ipv4_text(const uint8_t *text, uint16_t text_len, uint8_t value[4])
{
  uint32_t octet = 0U;
  uint8_t octet_count = 0U;
  bool have_digit = false;

  if ((text == 0) || (text_len == 0U) || (value == 0))
  {
    return false;
  }

  for (uint16_t i = 0U; i < text_len; i++)
  {
    const uint8_t ch = text[i];
    if ((ch >= (uint8_t)'0') && (ch <= (uint8_t)'9'))
    {
      have_digit = true;
      octet = (octet * 10U) + (uint32_t)(ch - (uint8_t)'0');
      if (octet > 255U)
      {
        return false;
      }
    }
    else if (ch == (uint8_t)'.')
    {
      if (!have_digit || (octet_count >= 3U))
      {
        return false;
      }
      value[octet_count++] = (uint8_t)octet;
      octet = 0U;
      have_digit = false;
    }
    else
    {
      return false;
    }
  }

  if (!have_digit || (octet_count != 3U))
  {
    return false;
  }

  value[octet_count] = (uint8_t)octet;
  return true;
}

static bool parse_u32_text(const uint8_t *text, uint16_t text_len, uint32_t *value)
{
  uint32_t parsed = 0U;

  if ((text == 0) || (text_len == 0U) || (value == 0))
  {
    return false;
  }

  for (uint16_t i = 0U; i < text_len; i++)
  {
    const uint8_t ch = text[i];
    if ((ch < (uint8_t)'0') || (ch > (uint8_t)'9'))
    {
      return false;
    }

    parsed = (parsed * 10U) + (uint32_t)(ch - (uint8_t)'0');
  }

  *value = parsed;
  return true;
}

static bool text_equals(const uint8_t *text, uint16_t text_len, const char *expected)
{
  const size_t expected_len = strlen(expected);
  return (text != 0) && (text_len == expected_len) &&
         (memcmp(text, expected, expected_len) == 0);
}

static bool parse_rail_mode_text(const uint8_t *text, uint16_t text_len, ResidentHardwareRailMode *mode)
{
  uint16_t start;
  uint16_t end;
  const uint8_t *trimmed;

  if (mode == 0)
  {
    return false;
  }

  if ((text == 0) || (text_len == 0U))
  {
    return false;
  }

  start = 0U;
  while ((start < text_len) && ((text[start] == (uint8_t)' ') || (text[start] == (uint8_t)'\t')))
  {
    start++;
  }
  end = text_len;
  while ((end > start) && ((text[end - 1U] == (uint8_t)' ') || (text[end - 1U] == (uint8_t)'\t')))
  {
    end--;
  }

  trimmed = text + start;
  text_len = (uint16_t)(end - start);
  if (text_len == 0U)
  {
    return false;
  }

  if (text_equals(trimmed, text_len, "Enabled"))
  {
    *mode = RESIDENT_HARDWARE_RAIL_MODE_ENABLED;
    return true;
  }

  if (text_equals(trimmed, text_len, "Disabled"))
  {
    *mode = RESIDENT_HARDWARE_RAIL_MODE_DISABLED;
    return true;
  }

  if (text_equals(trimmed, text_len, "Follow Estop"))
  {
    *mode = RESIDENT_HARDWARE_RAIL_MODE_FOLLOW_ESTOP;
    return true;
  }

  return false;
}

static bool parse_net_dhcp_text(const uint8_t *text, uint16_t text_len, uint8_t *enabled_out)
{
  uint16_t start;
  uint16_t end;
  const uint8_t *trimmed;

  if (enabled_out == 0)
  {
    return false;
  }

  if ((text == 0) || (text_len == 0U))
  {
    return false;
  }

  start = 0U;
  while ((start < text_len) && ((text[start] == (uint8_t)' ') || (text[start] == (uint8_t)'\t')))
  {
    start++;
  }
  end = text_len;
  while ((end > start) && ((text[end - 1U] == (uint8_t)' ') || (text[end - 1U] == (uint8_t)'\t')))
  {
    end--;
  }

  trimmed = text + start;
  text_len = (uint16_t)(end - start);
  if (text_len == 0U)
  {
    return false;
  }

  if (text_equals(trimmed, text_len, "Enabled"))
  {
    *enabled_out = 1U;
    return true;
  }

  if (text_equals(trimmed, text_len, "Disabled"))
  {
    *enabled_out = 0U;
    return true;
  }

  if ((text_len == 1U) && (trimmed[0] == (uint8_t)'1'))
  {
    *enabled_out = 1U;
    return true;
  }

  if ((text_len == 1U) && (trimmed[0] == (uint8_t)'0'))
  {
    *enabled_out = 0U;
    return true;
  }

  return false;
}

static int append_u16(uint8_t *response, uint16_t response_max, uint16_t *offset, uint16_t value)
{
  if ((*offset + sizeof(value)) > response_max)
  {
    return PROTO_RESULT_GENERIC;
  }

  memcpy(response + *offset, &value, sizeof(value));
  *offset += sizeof(value);
  return PROTO_RESULT_OK;
}

static int append_bytes(uint8_t *response, uint16_t response_max, uint16_t *offset,
                        const void *data, uint16_t data_len)
{
  if ((*offset + data_len) > response_max)
  {
    return PROTO_RESULT_GENERIC;
  }

  if (data_len != 0U)
  {
    memcpy(response + *offset, data, data_len);
    *offset += data_len;
  }

  return PROTO_RESULT_OK;
}

static int append_list_item(uint8_t *response, uint16_t response_max, uint16_t *offset,
                            const ResidentTreeListItem *item)
{
  char json[256];
  int written = snprintf(json, sizeof(json), "{\"Name\":\"%s\",\"Value\":\"%s\"",
                         item->name, item->value);
  if ((written < 0) || ((size_t)written >= sizeof(json)))
  {
    return PROTO_RESULT_GENERIC;
  }

  size_t json_len = (size_t)written;
  if (item->write_effect != 0)
  {
    written = snprintf(json + json_len, sizeof(json) - json_len,
                       ",\"WriteEffect\":\"%s\"", item->write_effect);
    if ((written < 0) || ((json_len + (size_t)written) >= sizeof(json)))
    {
      return PROTO_RESULT_GENERIC;
    }
    json_len += (size_t)written;
  }

  if (item->description != 0)
  {
    written = snprintf(json + json_len, sizeof(json) - json_len,
                       ",\"Description\":\"%s\"", item->description);
    if ((written < 0) || ((json_len + (size_t)written) >= sizeof(json)))
    {
      return PROTO_RESULT_GENERIC;
    }
    json_len += (size_t)written;
  }

  if (item->action)
  {
    written = snprintf(json + json_len, sizeof(json) - json_len, ",\"Action\":true");
    if ((written < 0) || ((json_len + (size_t)written) >= sizeof(json)))
    {
      return PROTO_RESULT_GENERIC;
    }
    json_len += (size_t)written;
  }

  if (item->unit != 0)
  {
    written = snprintf(json + json_len, sizeof(json) - json_len,
                       ",\"Unit\":\"%s\"", item->unit);
    if ((written < 0) || ((json_len + (size_t)written) >= sizeof(json)))
    {
      return PROTO_RESULT_GENERIC;
    }
    json_len += (size_t)written;
  }

  if (item->enum_values != 0)
  {
    written = snprintf(json + json_len, sizeof(json) - json_len,
                       ",\"Enum\":[%s]", item->enum_values);
    if ((written < 0) || ((json_len + (size_t)written) >= sizeof(json)))
    {
      return PROTO_RESULT_GENERIC;
    }
    json_len += (size_t)written;
  }

  written = snprintf(json + json_len, sizeof(json) - json_len, "}");
  if ((written < 0) || ((json_len + (size_t)written) >= sizeof(json)))
  {
    return PROTO_RESULT_GENERIC;
  }
  json_len += (size_t)written;

  if ((*offset + 5U + (uint16_t)json_len) > response_max)
  {
    return PROTO_RESULT_GENERIC;
  }

  response[(*offset)++] = item->id;
  response[(*offset)++] = item->has_children;
  response[(*offset)++] = item->access;
  if (append_u16(response, response_max, offset, (uint16_t)json_len) != PROTO_RESULT_OK)
  {
    return PROTO_RESULT_GENERIC;
  }
  return append_bytes(response, response_max, offset, json, (uint16_t)json_len);
}

static bool location_is_root(const uint8_t *location, uint8_t depth)
{
  (void)location;
  return depth == 0U;
}

static bool location_is_network(const uint8_t *location, uint8_t depth)
{
  return (depth == 1U) && (location[0] == TREE_ID_NETWORK);
}

static bool location_is_reboot(const uint8_t *location, uint8_t depth)
{
  return (depth == 1U) && (location[0] == TREE_ID_REBOOT);
}

static bool location_is_app(const uint8_t *location, uint8_t depth)
{
  return (depth == 1U) && (location[0] == TREE_ID_APP);
}

static bool location_is_app_action(const uint8_t *location, uint8_t depth)
{
  return (depth == 2U) && (location[0] == TREE_ID_APP) &&
         (location[1] >= 1U) && (location[1] <= g_app_mount.action_count);
}

static bool location_is_hardware(const uint8_t *location, uint8_t depth)
{
  return (depth == 1U) && (location[0] == TREE_ID_HARDWARE);
}

static bool location_is_cpu_temp(const uint8_t *location, uint8_t depth)
{
  return (depth == 2U) && (location[0] == TREE_ID_HARDWARE) &&
         (location[1] == TREE_ID_HARDWARE_CPU_TEMP);
}

static bool location_is_hardware_config(const uint8_t *location, uint8_t depth)
{
  return (depth == 2U) && (location[0] == TREE_ID_HARDWARE) &&
         (location[1] == TREE_ID_HARDWARE_CONFIG);
}

static bool location_is_hardware_poll_period(const uint8_t *location, uint8_t depth)
{
  return (depth == 3U) && (location[0] == TREE_ID_HARDWARE) &&
         (location[1] == TREE_ID_HARDWARE_CONFIG) &&
         (location[2] == TREE_ID_HARDWARE_POLL_PERIOD);
}

static bool location_is_estop(const uint8_t *location, uint8_t depth)
{
  return (depth == 2U) && (location[0] == TREE_ID_HARDWARE) &&
         (location[1] == TREE_ID_HARDWARE_ESTOP);
}

static bool location_is_button(const uint8_t *location, uint8_t depth)
{
  return (depth == 2U) && (location[0] == TREE_ID_HARDWARE) &&
         (location[1] == TREE_ID_HARDWARE_BUTTON);
}

static bool location_is_rail(const uint8_t *location, uint8_t depth)
{
  return (depth == 2U) && (location[0] == TREE_ID_HARDWARE) &&
         ((location[1] == TREE_ID_HARDWARE_RAIL_A) ||
          (location[1] == TREE_ID_HARDWARE_RAIL_B));
}

static bool location_is_rail_mode(const uint8_t *location, uint8_t depth)
{
  return (depth == 3U) && (location[0] == TREE_ID_HARDWARE) &&
         ((location[1] == TREE_ID_HARDWARE_RAIL_A) ||
          (location[1] == TREE_ID_HARDWARE_RAIL_B)) &&
         (location[2] == TREE_ID_HARDWARE_RAIL_MODE);
}

static bool location_is_rail_output(const uint8_t *location, uint8_t depth)
{
  return (depth == 3U) && (location[0] == TREE_ID_HARDWARE) &&
         ((location[1] == TREE_ID_HARDWARE_RAIL_A) ||
          (location[1] == TREE_ID_HARDWARE_RAIL_B)) &&
         (location[2] == TREE_ID_HARDWARE_RAIL_OUTPUT);
}

static ResidentHardwareRailId rail_id_for_location(const uint8_t *location)
{
  return (location[1] == TREE_ID_HARDWARE_RAIL_B) ? RESIDENT_HARDWARE_RAIL_B : RESIDENT_HARDWARE_RAIL_A;
}

static bool location_is_ipv4_leaf(const uint8_t *location, uint8_t depth)
{
  return (depth == 2U) && (location[0] == TREE_ID_NETWORK) &&
         ((location[1] == TREE_ID_IPV4_ADDRESS) || (location[1] == TREE_ID_IPV4_SUBNET) ||
          (location[1] == TREE_ID_IPV4_GATEWAY));
}

static bool location_is_ipv4_dhcp(const uint8_t *location, uint8_t depth)
{
  return (depth == 2U) && (location[0] == TREE_ID_NETWORK) && (location[1] == TREE_ID_IPV4_DHCP);
}

static bool location_is_mac(const uint8_t *location, uint8_t depth)
{
  return (depth == 2U) && (location[0] == TREE_ID_NETWORK) && (location[1] == TREE_ID_NETWORK_MAC);
}

static bool location_is_debug(const uint8_t *location, uint8_t depth)
{
  return (depth == 1U) && (location[0] == TREE_ID_DEBUG);
}

static bool location_is_debug_flash(const uint8_t *location, uint8_t depth)
{
  return (depth == 2U) && (location[0] == TREE_ID_DEBUG) &&
         (location[1] == TREE_ID_DEBUG_FLASH);
}

static bool location_is_flash_current_settings_slot(const uint8_t *location, uint8_t depth)
{
  return (depth == 3U) && (location[0] == TREE_ID_DEBUG) &&
         (location[1] == TREE_ID_DEBUG_FLASH) &&
         (location[2] == TREE_ID_FLASH_CURRENT_SETTINGS_SLOT);
}

static bool location_is_flash_bytes_used(const uint8_t *location, uint8_t depth)
{
  return (depth == 3U) && (location[0] == TREE_ID_DEBUG) &&
         (location[1] == TREE_ID_DEBUG_FLASH) && (location[2] == TREE_ID_FLASH_BYTES_USED);
}

static bool location_is_flash_bytes_remaining(const uint8_t *location, uint8_t depth)
{
  return (depth == 3U) && (location[0] == TREE_ID_DEBUG) &&
         (location[1] == TREE_ID_DEBUG_FLASH) && (location[2] == TREE_ID_FLASH_BYTES_REMAINING);
}

static bool location_is_flash_current_settings_phys_addr(const uint8_t *location, uint8_t depth)
{
  return (depth == 3U) && (location[0] == TREE_ID_DEBUG) &&
         (location[1] == TREE_ID_DEBUG_FLASH) &&
         (location[2] == TREE_ID_FLASH_CURRENT_SETTINGS_PHYS_ADDR);
}

static bool location_is_flash_current_settings_object_size(const uint8_t *location, uint8_t depth)
{
  return (depth == 3U) && (location[0] == TREE_ID_DEBUG) &&
         (location[1] == TREE_ID_DEBUG_FLASH) &&
         (location[2] == TREE_ID_FLASH_CURRENT_SETTINGS_OBJECT_SIZE);
}

static bool app_action_name_is_valid(const char *path)
{
  if ((path == 0) || (path[0] == '\0'))
  {
    return false;
  }

  for (const char *cursor = path; *cursor != '\0'; cursor++)
  {
    if (*cursor == '/')
    {
      return false;
    }
  }

  return true;
}

static int execute_reboot(const uint8_t *args, uint16_t args_len,
                          uint8_t *response, uint16_t response_max, uint16_t *response_len)
{
  (void)args;
  (void)args_len;
  (void)response;
  (void)response_max;

  if (response_len == 0)
  {
    return PROTO_RESULT_PARSE;
  }

  *response_len = 0U;
  g_reboot_requested = true;
  return PROTO_RESULT_OK;
}

static void reboot_task(void *argument)
{
  (void)argument;

  for (;;)
  {
    if (g_reboot_requested)
    {
      g_reboot_requested = false;
      resident_network_prepare_for_reset();
      HAL_NVIC_SystemReset();
    }

    osDelay(10U);
  }
}

void resident_device_tree_init(void)
{
  memset(&g_app_mount, 0, sizeof(g_app_mount));
  g_reboot_requested = false;

  const osThreadAttr_t task_attr = {
    .name = "TreeReboot",
    .cb_mem = &g_reboot_task_cb,
    .cb_size = sizeof(g_reboot_task_cb),
    .stack_mem = g_reboot_task_stack,
    .stack_size = sizeof(g_reboot_task_stack),
    .priority = osPriorityLow,
  };
  (void)osThreadNew(reboot_task, 0, &task_attr);
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
  if ((mount != &g_app_mount) || !g_app_mount.mounted ||
      !app_action_name_is_valid(path) || (callback == 0))
  {
    return -1;
  }

  for (uint8_t i = 0U; i < g_app_mount.action_count; i++)
  {
    if (strncmp(g_app_mount.actions[i].path, path, sizeof(g_app_mount.actions[i].path)) == 0)
    {
      g_app_mount.actions[i].callback = callback;
      g_app_mount.actions[i].context = context;
      return 0;
    }
  }

  if (g_app_mount.action_count >= TREE_APP_ACTION_MAX)
  {
    return -1;
  }

  ResidentAppTreeAction *action = &g_app_mount.actions[g_app_mount.action_count++];
  (void)snprintf(action->path, sizeof(action->path), "%s", path);
  action->callback = callback;
  action->context = context;
  return 0;
}

int resident_device_tree_list(const uint8_t *location, uint8_t depth,
                              uint8_t *response, uint16_t response_max, uint16_t *response_len)
{
  uint16_t offset = 0U;
  ResidentTreeListItem items[TREE_APP_ACTION_MAX + 8U];
  uint16_t count = 0U;
  uint8_t ip[4];
  uint8_t subnet[4];
  uint8_t gateway[4];
  char mac_text[18];
  char ip_text[16];
  char subnet_text[16];
  char gateway_text[16];
  char cpu_temp_text[16];
  char poll_period_text[12];
  char estop_text[6];
  char button_text[6];
  char rail_a_mode_text[16];
  char rail_b_mode_text[16];
  char rail_a_output_text[16];
  char rail_b_output_text[16];
  char dhcp_text[12];
  char flash_current_slot_text[12];
  char flash_bytes_used_text[24];
  char flash_bytes_remaining_text[24];
  char flash_settings_phys_addr_text[20];
  char flash_settings_obj_size_text[16];

  if ((response == 0) || (response_len == 0) || ((depth != 0U) && (location == 0)))
  {
    return PROTO_RESULT_PARSE;
  }

  if (boot_metadata_get_net_dhcp_enabled() != 0U)
  {
    (void)resident_network_get_ipv4(ip, subnet, gateway);
  }
  else
  {
    boot_metadata_get_ipv4(ip, subnet, gateway);
  }
  (void)snprintf(dhcp_text, sizeof(dhcp_text), "%s",
                 (boot_metadata_get_net_dhcp_enabled() != 0U) ? "Enabled" : "Disabled");
  format_mac(mac_text, sizeof(mac_text));
  format_ipv4(ip, ip_text, sizeof(ip_text));
  format_ipv4(subnet, subnet_text, sizeof(subnet_text));
  format_ipv4(gateway, gateway_text, sizeof(gateway_text));

  (void)snprintf(flash_current_slot_text, sizeof(flash_current_slot_text), "%lu",
                 (unsigned long)boot_metadata_get_current_settings_slot());
  (void)snprintf(flash_bytes_used_text, sizeof(flash_bytes_used_text), "%lu",
                 (unsigned long)boot_metadata_get_flash_bytes_used());
  (void)snprintf(flash_bytes_remaining_text, sizeof(flash_bytes_remaining_text), "%lu",
                 (unsigned long)boot_metadata_get_flash_bytes_remaining());
  (void)snprintf(flash_settings_phys_addr_text, sizeof(flash_settings_phys_addr_text), "0x%08lX",
                 (unsigned long)boot_metadata_get_current_settings_object_phys_addr());
  (void)snprintf(flash_settings_obj_size_text, sizeof(flash_settings_obj_size_text), "%lu",
                 (unsigned long)boot_metadata_get_current_settings_object_size_bytes());

  if (location_is_root(location, depth))
  {
    items[count++] = (ResidentTreeListItem){TREE_ID_NETWORK, 1U, TREE_ACCESS_READ, "Network", ""};
    items[count++] = (ResidentTreeListItem){TREE_ID_HARDWARE, 1U, TREE_ACCESS_READ, "Hardware", ""};
    items[count++] = (ResidentTreeListItem){TREE_ID_DEBUG, 1U, TREE_ACCESS_READ, "Debug", ""};
    items[count++] = (ResidentTreeListItem){TREE_ID_REBOOT, 0U, TREE_ACCESS_EXECUTE, "Reboot", "",
                                            0, "Reset the device after replying", true};
    if (g_app_mount.mounted)
    {
      items[count++] = (ResidentTreeListItem){TREE_ID_APP, 1U, TREE_ACCESS_READ, g_app_mount.name, ""};
    }
  }
  else if (location_is_network(location, depth))
  {
    const uint8_t dhcp_on = boot_metadata_get_net_dhcp_enabled();
    items[count++] = (ResidentTreeListItem){TREE_ID_NETWORK_MAC, 0U, TREE_ACCESS_READ_WRITE,
                                            "MAC", mac_text, TREE_WRITE_EFFECT_REBOOT};
    items[count++] = (ResidentTreeListItem){TREE_ID_IPV4_DHCP, 0U, TREE_ACCESS_READ_WRITE,
                                            "DHCP", dhcp_text, TREE_WRITE_EFFECT_REBOOT,
                                            "DHCP preference (applied after reboot)",
                                            false, 0, TREE_NET_DHCP_ENUM_JSON};
    items[count++] =
        (ResidentTreeListItem){TREE_ID_IPV4_ADDRESS, 0U,
                               (dhcp_on != 0U) ? TREE_ACCESS_READ : TREE_ACCESS_READ_WRITE,
                               "Address", ip_text,
                               (dhcp_on != 0U) ? (const char *)0 : TREE_WRITE_EFFECT_REBOOT,
                               (dhcp_on != 0U) ? "Runtime IPv4 address (DHCP)" : (const char *)0};
    items[count++] =
        (ResidentTreeListItem){TREE_ID_IPV4_SUBNET, 0U,
                               (dhcp_on != 0U) ? TREE_ACCESS_READ : TREE_ACCESS_READ_WRITE,
                               "Subnet", subnet_text,
                               (dhcp_on != 0U) ? (const char *)0 : TREE_WRITE_EFFECT_REBOOT,
                               (dhcp_on != 0U) ? "Runtime subnet mask (DHCP)" : (const char *)0};
    items[count++] =
        (ResidentTreeListItem){TREE_ID_IPV4_GATEWAY, 0U,
                               (dhcp_on != 0U) ? TREE_ACCESS_READ : TREE_ACCESS_READ_WRITE,
                               "Gateway", gateway_text,
                               (dhcp_on != 0U) ? (const char *)0 : TREE_WRITE_EFFECT_REBOOT,
                               (dhcp_on != 0U) ? "Runtime default gateway (DHCP)" : (const char *)0};
  }
  else if (location_is_hardware(location, depth))
  {
    (void)resident_hardware_get_cpu_temp_text(cpu_temp_text, sizeof(cpu_temp_text));
    (void)resident_hardware_get_estop_text(estop_text, sizeof(estop_text));
    (void)resident_hardware_get_button_text(button_text, sizeof(button_text));
    items[count++] = (ResidentTreeListItem){TREE_ID_HARDWARE_CPU_TEMP, 0U, TREE_ACCESS_READ,
                                            "CPU Temp", cpu_temp_text,
                                            0, "Embedded temperature sensor",
                                            false, "C"};
    items[count++] = (ResidentTreeListItem){TREE_ID_HARDWARE_CONFIG, 1U, TREE_ACCESS_READ, "Config", ""};
    items[count++] = (ResidentTreeListItem){TREE_ID_HARDWARE_ESTOP, 0U, TREE_ACCESS_READ,
                                            "EstopInput", estop_text};
    items[count++] = (ResidentTreeListItem){TREE_ID_HARDWARE_RAIL_A, 1U, TREE_ACCESS_READ, "Rail A", ""};
    items[count++] = (ResidentTreeListItem){TREE_ID_HARDWARE_RAIL_B, 1U, TREE_ACCESS_READ, "Rail B", ""};
    items[count++] = (ResidentTreeListItem){TREE_ID_HARDWARE_BUTTON, 0U, TREE_ACCESS_READ,
                                            "Button Input", button_text};
  }
  else if (location_is_hardware_config(location, depth))
  {
    (void)resident_hardware_get_poll_period_text(poll_period_text, sizeof(poll_period_text));
    items[count++] = (ResidentTreeListItem){TREE_ID_HARDWARE_POLL_PERIOD, 0U, TREE_ACCESS_READ_WRITE,
                                            "Hardware Poll Period (ms)", poll_period_text,
                                            0, "Background hardware cache refresh period",
                                            false, "ms"};
  }
  else if (location_is_rail(location, depth))
  {
    const bool is_rail_b = (location[1] == TREE_ID_HARDWARE_RAIL_B);
    (void)resident_hardware_get_rail_mode_text(is_rail_b ? RESIDENT_HARDWARE_RAIL_B : RESIDENT_HARDWARE_RAIL_A,
                                               is_rail_b ? rail_b_mode_text : rail_a_mode_text,
                                               is_rail_b ? sizeof(rail_b_mode_text) : sizeof(rail_a_mode_text));
    (void)resident_hardware_get_rail_output_text(is_rail_b ? RESIDENT_HARDWARE_RAIL_B : RESIDENT_HARDWARE_RAIL_A,
                                                 is_rail_b ? rail_b_output_text : rail_a_output_text,
                                                 is_rail_b ? sizeof(rail_b_output_text) : sizeof(rail_a_output_text));

    items[count++] = (ResidentTreeListItem){TREE_ID_HARDWARE_RAIL_MODE, 0U, TREE_ACCESS_READ_WRITE,
                                            "Mode", is_rail_b ? rail_b_mode_text : rail_a_mode_text,
                                            0, "Rail output mode",
                                            false, 0, TREE_RAIL_MODE_ENUM_JSON};
    items[count++] = (ResidentTreeListItem){TREE_ID_HARDWARE_RAIL_OUTPUT, 0U, TREE_ACCESS_READ,
                                            "Output", is_rail_b ? rail_b_output_text : rail_a_output_text,
                                            0, "Measured rail current",
                                            false, "Amps"};
  }
  else if (location_is_debug(location, depth))
  {
    items[count++] = (ResidentTreeListItem){TREE_ID_DEBUG_FLASH, 1U, TREE_ACCESS_READ, "Flash", ""};
  }
  else if (location_is_debug_flash(location, depth))
  {
    items[count++] = (ResidentTreeListItem){TREE_ID_FLASH_CURRENT_SETTINGS_SLOT, 0U, TREE_ACCESS_READ,
                                            "Current Settings Slot", flash_current_slot_text,
                                            0, "Index of the active metadata copy in flash (0-based)"};
    items[count++] = (ResidentTreeListItem){TREE_ID_FLASH_BYTES_USED, 0U, TREE_ACCESS_READ,
                                            "Bytes Used (metadata sector)", flash_bytes_used_text,
                                            0,
                                            "Bytes consumed from the metadata sector heap (bitmap + blobs)"};
    items[count++] = (ResidentTreeListItem){TREE_ID_FLASH_BYTES_REMAINING, 0U, TREE_ACCESS_READ,
                                            "Bytes Remaining (sector)", flash_bytes_remaining_text,
                                            0,
                                            "Bytes remaining in the metadata flash sector"};
    items[count++] = (ResidentTreeListItem){TREE_ID_FLASH_CURRENT_SETTINGS_PHYS_ADDR, 0U, TREE_ACCESS_READ,
                                            "Current Settings Object Address", flash_settings_phys_addr_text,
                                            0,
                                            "Absolute flash base address of the active KV/settings record"};
    items[count++] = (ResidentTreeListItem){TREE_ID_FLASH_CURRENT_SETTINGS_OBJECT_SIZE, 0U, TREE_ACCESS_READ,
                                            "Current Settings Object Size", flash_settings_obj_size_text,
                                            0,
                                            "KV blob record_total: logical byte length including trailing CRC"};
  }
  else if (location_is_app(location, depth) && g_app_mount.mounted)
  {
    for (uint8_t i = 0U; i < g_app_mount.action_count; i++)
    {
      items[count++] = (ResidentTreeListItem){(uint8_t)(i + 1U), 0U, TREE_ACCESS_EXECUTE,
                                              g_app_mount.actions[i].path, "",
                                              0, "Application action", true};
    }
  }
  else
  {
    return PROTO_RESULT_NOT_FOUND;
  }

  if (append_u16(response, response_max, &offset, count) != PROTO_RESULT_OK)
  {
    return PROTO_RESULT_GENERIC;
  }

  for (uint16_t i = 0U; i < count; i++)
  {
    const int result = append_list_item(response, response_max, &offset, &items[i]);
    if (result != PROTO_RESULT_OK)
    {
      return result;
    }
  }

  *response_len = offset;
  return PROTO_RESULT_OK;
}

int resident_device_tree_get(const uint8_t *location, uint8_t depth,
                             uint8_t *response, uint16_t response_max, uint16_t *response_len)
{
  char value[32];
  uint16_t value_len;
  uint16_t offset = 0U;
  uint8_t ip[4];
  uint8_t subnet[4];
  uint8_t gateway[4];

  if ((location == 0) || (response == 0) || (response_len == 0))
  {
    return PROTO_RESULT_PARSE;
  }

  if (location_is_mac(location, depth))
  {
    format_mac(value, sizeof(value));
  }
  else if (location_is_cpu_temp(location, depth))
  {
    (void)resident_hardware_get_cpu_temp_text(value, sizeof(value));
  }
  else if (location_is_hardware_poll_period(location, depth))
  {
    (void)resident_hardware_get_poll_period_text(value, sizeof(value));
  }
  else if (location_is_estop(location, depth))
  {
    (void)resident_hardware_get_estop_text(value, sizeof(value));
  }
  else if (location_is_button(location, depth))
  {
    (void)resident_hardware_get_button_text(value, sizeof(value));
  }
  else if (location_is_rail_mode(location, depth))
  {
    (void)resident_hardware_get_rail_mode_text(rail_id_for_location(location), value, sizeof(value));
  }
  else if (location_is_rail_output(location, depth))
  {
    (void)resident_hardware_get_rail_output_text(rail_id_for_location(location), value, sizeof(value));
  }
  else if (location_is_ipv4_dhcp(location, depth))
  {
    (void)snprintf(value, sizeof(value), "%s",
                   (boot_metadata_get_net_dhcp_enabled() != 0U) ? "Enabled" : "Disabled");
  }
  else if (location_is_ipv4_leaf(location, depth))
  {
    if (boot_metadata_get_net_dhcp_enabled() != 0U)
    {
      (void)resident_network_get_ipv4(ip, subnet, gateway);
    }
    else
    {
      boot_metadata_get_ipv4(ip, subnet, gateway);
    }
    const uint8_t *selected = ip;
    if (location[1] == TREE_ID_IPV4_SUBNET)
    {
      selected = subnet;
    }
    else if (location[1] == TREE_ID_IPV4_GATEWAY)
    {
      selected = gateway;
    }
    format_ipv4(selected, value, sizeof(value));
  }
  else if (location_is_flash_current_settings_slot(location, depth))
  {
    (void)snprintf(value, sizeof(value), "%lu",
                   (unsigned long)boot_metadata_get_current_settings_slot());
  }
  else if (location_is_flash_bytes_used(location, depth))
  {
    (void)snprintf(value, sizeof(value), "%lu",
                   (unsigned long)boot_metadata_get_flash_bytes_used());
  }
  else if (location_is_flash_bytes_remaining(location, depth))
  {
    (void)snprintf(value, sizeof(value), "%lu",
                   (unsigned long)boot_metadata_get_flash_bytes_remaining());
  }
  else if (location_is_flash_current_settings_phys_addr(location, depth))
  {
    (void)snprintf(value, sizeof(value), "0x%08lX",
                   (unsigned long)boot_metadata_get_current_settings_object_phys_addr());
  }
  else if (location_is_flash_current_settings_object_size(location, depth))
  {
    (void)snprintf(value, sizeof(value), "%lu",
                   (unsigned long)boot_metadata_get_current_settings_object_size_bytes());
  }
  else if (location_is_reboot(location, depth) || location_is_app_action(location, depth))
  {
    return PROTO_RESULT_INVALID_VALUE;
  }
  else
  {
    return PROTO_RESULT_NOT_FOUND;
  }

  value_len = (uint16_t)strlen(value);
  if (append_u16(response, response_max, &offset, value_len) != PROTO_RESULT_OK)
  {
    return PROTO_RESULT_GENERIC;
  }
  if (append_bytes(response, response_max, &offset, value, value_len) != PROTO_RESULT_OK)
  {
    return PROTO_RESULT_GENERIC;
  }

  *response_len = offset;
  return PROTO_RESULT_OK;
}

int resident_device_tree_set(const uint8_t *location, uint8_t depth,
                             const uint8_t *value, uint16_t value_len,
                             uint8_t *response, uint16_t response_max, uint16_t *response_len)
{
  uint8_t ip[4];
  uint8_t subnet[4];
  uint8_t gateway[4];
  uint8_t parsed_ip[4];
  uint8_t parsed_mac[6];
  uint32_t parsed_u32;
  ResidentHardwareRailMode parsed_mode;
  uint8_t parsed_dhcp_enabled;

  if ((location == 0) || (value == 0) || (response == 0) || (response_len == 0))
  {
    return PROTO_RESULT_PARSE;
  }

  if (location_is_mac(location, depth))
  {
    if (!parse_mac_text(value, value_len, parsed_mac))
    {
      return PROTO_RESULT_INVALID_VALUE;
    }

    if (boot_metadata_set_mac(parsed_mac) != 0)
    {
      return PROTO_RESULT_GENERIC;
    }

    if (response_max < 1U)
    {
      return PROTO_RESULT_GENERIC;
    }
    response[0] = 1U;
    *response_len = 1U;
    return PROTO_RESULT_OK;
  }

  if (location_is_ipv4_dhcp(location, depth))
  {
    if (!parse_net_dhcp_text(value, value_len, &parsed_dhcp_enabled))
    {
      return PROTO_RESULT_INVALID_VALUE;
    }

    const uint8_t cur = boot_metadata_get_net_dhcp_enabled();
    if (cur == parsed_dhcp_enabled)
    {
      if (response_max < 1U)
      {
        return PROTO_RESULT_GENERIC;
      }
      response[0] = 0U;
      *response_len = 1U;
      return PROTO_RESULT_OK;
    }

    if (boot_metadata_set_net_dhcp_enabled(parsed_dhcp_enabled) != 0)
    {
      return PROTO_RESULT_GENERIC;
    }

    if (response_max < 1U)
    {
      return PROTO_RESULT_GENERIC;
    }
    response[0] = 1U;
    *response_len = 1U;
    return PROTO_RESULT_OK;
  }

  if (location_is_hardware_poll_period(location, depth))
  {
    if (!parse_u32_text(value, value_len, &parsed_u32))
    {
      return PROTO_RESULT_INVALID_VALUE;
    }

    if (boot_metadata_set_hardware_poll_period_ms(parsed_u32) != 0)
    {
      return PROTO_RESULT_GENERIC;
    }

    (void)resident_hardware_set_poll_period_ms(boot_metadata_get_hardware_poll_period_ms());
    if (response_max < 1U)
    {
      return PROTO_RESULT_GENERIC;
    }
    response[0] = 0U;
    *response_len = 1U;
    return PROTO_RESULT_OK;
  }

  if (location_is_rail_mode(location, depth))
  {
    if (!parse_rail_mode_text(value, value_len, &parsed_mode))
    {
      return PROTO_RESULT_INVALID_VALUE;
    }

    if (resident_hardware_set_rail_mode(rail_id_for_location(location), parsed_mode) != 0)
    {
      return PROTO_RESULT_GENERIC;
    }

    if (response_max < 1U)
    {
      return PROTO_RESULT_GENERIC;
    }
    response[0] = 0U;
    *response_len = 1U;
    return PROTO_RESULT_OK;
  }

  if (!location_is_ipv4_leaf(location, depth))
  {
    if (location_is_reboot(location, depth) || location_is_app_action(location, depth) ||
        location_is_cpu_temp(location, depth) || location_is_estop(location, depth) ||
        location_is_button(location, depth) || location_is_rail_output(location, depth) ||
        location_is_flash_current_settings_slot(location, depth) ||
        location_is_flash_bytes_used(location, depth) ||
        location_is_flash_bytes_remaining(location, depth) ||
        location_is_flash_current_settings_phys_addr(location, depth) ||
        location_is_flash_current_settings_object_size(location, depth))
    {
      return PROTO_RESULT_INVALID_VALUE;
    }
    return PROTO_RESULT_NOT_FOUND;
  }

  if (boot_metadata_get_net_dhcp_enabled() != 0U)
  {
    return PROTO_RESULT_INVALID_VALUE;
  }

  if (!parse_ipv4_text(value, value_len, parsed_ip))
  {
    return PROTO_RESULT_INVALID_VALUE;
  }

  if (boot_metadata_ipv4_is_unusable_reserved(parsed_ip))
  {
    return PROTO_RESULT_INVALID_VALUE;
  }

  boot_metadata_get_ipv4(ip, subnet, gateway);
  if (location[1] == TREE_ID_IPV4_ADDRESS)
  {
    memcpy(ip, parsed_ip, sizeof(ip));
  }
  else if (location[1] == TREE_ID_IPV4_SUBNET)
  {
    memcpy(subnet, parsed_ip, sizeof(subnet));
  }
  else
  {
    memcpy(gateway, parsed_ip, sizeof(gateway));
  }

  if (boot_metadata_set_ipv4(ip, subnet, gateway) != 0)
  {
    return PROTO_RESULT_GENERIC;
  }

  if (response_max < 1U)
  {
    return PROTO_RESULT_GENERIC;
  }
  response[0] = 1U;
  *response_len = 1U;
  return PROTO_RESULT_OK;
}

int resident_device_tree_execute(const uint8_t *location, uint8_t depth,
                                 const uint8_t *args, uint16_t args_len,
                                 uint8_t *response, uint16_t response_max, uint16_t *response_len)
{
  char args_text[128];

  if ((location == 0) || (response == 0) || (response_len == 0) ||
      ((args_len != 0U) && (args == 0)))
  {
    return PROTO_RESULT_PARSE;
  }

  if (location_is_reboot(location, depth))
  {
    return execute_reboot(args, args_len, response, response_max, response_len);
  }

  if (location_is_app_action(location, depth) && g_app_mount.mounted)
  {
    ResidentAppTreeAction *action = &g_app_mount.actions[location[1] - 1U];
    if (action->callback == 0)
    {
      return PROTO_RESULT_NOT_FOUND;
    }

    if (args_len >= sizeof(args_text))
    {
      return PROTO_RESULT_INVALID_VALUE;
    }

    if (args_len != 0U)
    {
      memcpy(args_text, args, args_len);
    }
    args_text[args_len] = '\0';
    *response_len = 0U;

    const int callback_result = action->callback(args_text, action->context);
    return (callback_result == 0) ? PROTO_RESULT_OK : PROTO_RESULT_GENERIC;
  }

  if (location_is_mac(location, depth) || location_is_ipv4_leaf(location, depth) ||
      location_is_ipv4_dhcp(location, depth) ||
      location_is_cpu_temp(location, depth) || location_is_hardware_poll_period(location, depth) ||
      location_is_estop(location, depth) || location_is_button(location, depth) ||
      location_is_rail_mode(location, depth) || location_is_rail_output(location, depth) ||
      location_is_flash_current_settings_slot(location, depth) ||
      location_is_flash_bytes_used(location, depth) || location_is_flash_bytes_remaining(location, depth) ||
      location_is_flash_current_settings_phys_addr(location, depth) ||
      location_is_flash_current_settings_object_size(location, depth))
  {
    return PROTO_RESULT_INVALID_VALUE;
  }

  return PROTO_RESULT_NOT_FOUND;
}
