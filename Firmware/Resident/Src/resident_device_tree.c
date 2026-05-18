#include "resident_device_tree.h"

#include "boot_metadata.h"
#include "cmsis_os.h"
#include "lwip/netif.h"
#include "proto_common.h"
#include "resident_program_manager.h"
#include "resident_tree_core.h"
#include "resident_tree_types.h"
#include "resident_tree_network.h"
#include "resident_tree_hardware.h"
#include "resident_tree_debug.h"
#include "resident_tree_app.h"
#include "resident_tree_root.h"
#include "resident_hardware.h"
#include "resident_network.h"
#include "resident_text.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

extern struct netif gnetif;

#define TREE_ID_NETWORK                 (1U)
#define TREE_ID_PROGRAM                 (2U)
#define TREE_ID_HARDWARE                (3U)
#define TREE_ID_DEBUG                   (4U)
#define TREE_ID_REBOOT                  (5U)
#define TREE_ID_APP                     (6U)
#define TREE_ID_PROGRAM_STATE           (1U)
#define TREE_ID_PROGRAM_TCP_PORT        (2U)
#define TREE_ID_PROGRAM_APPLY_BOOT      (3U)
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
#define TREE_PROGRAM_STATE_ENUM_JSON    "\"Erasing\",\"ProgrammingReady\",\"Stopped\",\"Paused\",\"Running\""

static bool list_static_node(const ResidentTreeNode *node, const uint8_t *parent_location,
                             ResidentTreeListItem *item);
static bool list_mac_node(const ResidentTreeNode *node, const uint8_t *parent_location,
                          ResidentTreeListItem *item);
static bool list_dhcp_node(const ResidentTreeNode *node, const uint8_t *parent_location,
                           ResidentTreeListItem *item);
static bool list_ipv4_node(const ResidentTreeNode *node, const uint8_t *parent_location,
                           ResidentTreeListItem *item);
static bool list_hardware_text_node(const ResidentTreeNode *node, const uint8_t *parent_location,
                                    ResidentTreeListItem *item);
static bool list_rail_text_node(const ResidentTreeNode *node, const uint8_t *parent_location,
                                ResidentTreeListItem *item);
static bool list_flash_stat_node(const ResidentTreeNode *node, const uint8_t *parent_location,
                                 ResidentTreeListItem *item);
static bool list_app_root_node(const ResidentTreeNode *node, const uint8_t *parent_location,
                               ResidentTreeListItem *item);
static bool list_app_action_node(const ResidentTreeNode *node, const uint8_t *parent_location,
                                 ResidentTreeListItem *item);
static bool list_flash_kv_node(const ResidentTreeNode *node, const uint8_t *parent_location,
                               ResidentTreeListItem *item);
static bool list_program_node(const ResidentTreeNode *node, const uint8_t *parent_location,
                              ResidentTreeListItem *item);
static int execute_reboot_node(const ResidentTreeNode *node, const uint8_t *location,
                               uint8_t depth, const uint8_t *args, uint16_t args_len,
                               uint8_t *response, uint16_t response_max, uint16_t *response_len);
static int execute_app_action_node(const ResidentTreeNode *node, const uint8_t *location,
                                   uint8_t depth, const uint8_t *args, uint16_t args_len,
                                   uint8_t *response, uint16_t response_max, uint16_t *response_len);
static int execute_program_apply_boot_node(const ResidentTreeNode *node, const uint8_t *location,
                                           uint8_t depth, const uint8_t *args, uint16_t args_len,
                                           uint8_t *response, uint16_t response_max, uint16_t *response_len);

static ResidentTreeAppMount g_app_mount;
static volatile bool g_reboot_requested;
static StaticTask_t g_reboot_task_cb;
static StackType_t g_reboot_task_stack[96];
static ResidentTreeListItem g_list_items[BOOT_METADATA_STORED_KV_MAX + 8U];
static char g_list_value_text[8][64];
static char g_flash_kv_names[BOOT_METADATA_STORED_KV_MAX][16];
static char g_flash_kv_values[BOOT_METADATA_STORED_KV_MAX][64];
static ResidentTreeNode g_app_action_nodes[RESIDENT_TREE_APP_ACTION_MAX];
static ResidentTreeNode g_flash_kv_nodes[BOOT_METADATA_STORED_KV_MAX];

static ResidentTreeNode k_node_network_mac = {
  TREE_ID_NETWORK_MAC, TREE_NODE_STATIC, 0, 0, 0, list_mac_node, "MAC", TREE_ACCESS_READ_WRITE,
  TREE_WRITE_EFFECT_REBOOT, 0, false, 0, 0};
static ResidentTreeNode k_node_ipv4_address = {
  TREE_ID_IPV4_ADDRESS, TREE_NODE_STATIC, 0, 0, 0, list_ipv4_node, "Address", TREE_ACCESS_READ_WRITE,
  TREE_WRITE_EFFECT_REBOOT, 0, false, 0, 0};
static ResidentTreeNode k_node_ipv4_subnet = {
  TREE_ID_IPV4_SUBNET, TREE_NODE_STATIC, 0, 0, 0, list_ipv4_node, "Subnet", TREE_ACCESS_READ_WRITE,
  TREE_WRITE_EFFECT_REBOOT, 0, false, 0, 0};
static ResidentTreeNode k_node_ipv4_gateway = {
  TREE_ID_IPV4_GATEWAY, TREE_NODE_STATIC, 0, 0, 0, list_ipv4_node, "Gateway", TREE_ACCESS_READ_WRITE,
  TREE_WRITE_EFFECT_REBOOT, 0, false, 0, 0};
static ResidentTreeNode k_node_ipv4_dhcp = {
  TREE_ID_IPV4_DHCP, TREE_NODE_STATIC, 0, 0, 0, list_dhcp_node, "DHCP", TREE_ACCESS_READ_WRITE,
  TREE_WRITE_EFFECT_REBOOT, "DHCP preference (applied after reboot)", false, 0, TREE_NET_DHCP_ENUM_JSON};
static ResidentTreeNode k_node_network = {
  TREE_ID_NETWORK, TREE_NODE_STATIC, 0, &k_node_network_mac, 0, list_static_node, "Network",
  TREE_ACCESS_READ, 0, 0, false, 0, 0};

static ResidentTreeNode k_node_program_state = {
  TREE_ID_PROGRAM_STATE, TREE_NODE_STATIC, 0, 0, 0, list_program_node, "State",
  TREE_ACCESS_READ_WRITE, 0, "Program lifecycle state", false, 0, TREE_PROGRAM_STATE_ENUM_JSON};
static ResidentTreeNode k_node_program_tcp_port = {
  TREE_ID_PROGRAM_TCP_PORT, TREE_NODE_STATIC, 0, 0, 0, list_program_node, "Programming TCP port",
  TREE_ACCESS_READ, 0, "-1 until TCP programming is ready", false, 0, 0};
static ResidentTreeNode k_node_program_apply_boot = {
  TREE_ID_PROGRAM_APPLY_BOOT, TREE_NODE_STATIC, 0, 0, execute_program_apply_boot_node, list_program_node,
  "Apply Boot Update", TREE_ACCESS_EXECUTE, 0,
  "Start IAP app from sectors 6-8 to copy payload sectors 9-10 onto resident sectors 0-5", true, 0, 0};
static ResidentTreeNode k_node_program = {
  TREE_ID_PROGRAM, TREE_NODE_STATIC, 0, &k_node_program_state, 0, list_static_node, "Program",
  TREE_ACCESS_READ, 0, 0, false, 0, 0};

static ResidentTreeNode k_node_hardware_cpu_temp = {
  TREE_ID_HARDWARE_CPU_TEMP, TREE_NODE_STATIC, 0, 0, 0, list_hardware_text_node, "CPU Temp",
  TREE_ACCESS_READ, 0, "Embedded temperature sensor", false, "C", 0};
static ResidentTreeNode k_node_hardware_poll_period = {
  TREE_ID_HARDWARE_POLL_PERIOD, TREE_NODE_STATIC, 0, 0, 0, list_hardware_text_node,
  "Hardware Poll Period (ms)", TREE_ACCESS_READ_WRITE, 0, "Background hardware cache refresh period",
  false, "ms", 0};
static ResidentTreeNode k_node_hardware_config = {
  TREE_ID_HARDWARE_CONFIG, TREE_NODE_STATIC, 0, &k_node_hardware_poll_period, 0, list_static_node,
  "Config", TREE_ACCESS_READ, 0, 0, false, 0, 0};
static ResidentTreeNode k_node_hardware_estop = {
  TREE_ID_HARDWARE_ESTOP, TREE_NODE_STATIC, 0, 0, 0, list_hardware_text_node, "EstopInput",
  TREE_ACCESS_READ, 0, 0, false, 0, 0};
static ResidentTreeNode k_node_rail_mode = {
  TREE_ID_HARDWARE_RAIL_MODE, TREE_NODE_STATIC, 0, 0, 0, list_rail_text_node, "Mode",
  TREE_ACCESS_READ_WRITE, 0, "Rail output mode", false, 0, TREE_RAIL_MODE_ENUM_JSON};
static ResidentTreeNode k_node_rail_output = {
  TREE_ID_HARDWARE_RAIL_OUTPUT, TREE_NODE_STATIC, 0, 0, 0, list_rail_text_node, "Output",
  TREE_ACCESS_READ, 0, "Measured rail current", false, "Amps", 0};
static ResidentTreeNode k_node_hardware_rail_a = {
  TREE_ID_HARDWARE_RAIL_A, TREE_NODE_STATIC, 0, &k_node_rail_mode, 0, list_static_node, "Rail A",
  TREE_ACCESS_READ, 0, 0, false, 0, 0};
static ResidentTreeNode k_node_hardware_rail_b = {
  TREE_ID_HARDWARE_RAIL_B, TREE_NODE_STATIC, 0, &k_node_rail_mode, 0, list_static_node, "Rail B",
  TREE_ACCESS_READ, 0, 0, false, 0, 0};
static ResidentTreeNode k_node_hardware_button = {
  TREE_ID_HARDWARE_BUTTON, TREE_NODE_STATIC, 0, 0, 0, list_hardware_text_node, "Button Input",
  TREE_ACCESS_READ, 0, 0, false, 0, 0};
static ResidentTreeNode k_node_hardware = {
  TREE_ID_HARDWARE, TREE_NODE_STATIC, 0, &k_node_hardware_cpu_temp, 0, list_static_node, "Hardware",
  TREE_ACCESS_READ, 0, 0, false, 0, 0};

static ResidentTreeNode k_node_flash_current_settings_slot = {
  TREE_ID_FLASH_CURRENT_SETTINGS_SLOT, TREE_NODE_STATIC, 0, 0, 0, list_flash_stat_node,
  "Current Settings Slot", TREE_ACCESS_READ, 0, "Index of the active metadata copy in flash (0-based)",
  false, 0, 0};
static ResidentTreeNode k_node_flash_bytes_used = {
  TREE_ID_FLASH_BYTES_USED, TREE_NODE_STATIC, 0, 0, 0, list_flash_stat_node,
  "Bytes Used (metadata sector)", TREE_ACCESS_READ, 0,
  "Bytes consumed from the metadata sector heap (bitmap + blobs)", false, 0, 0};
static ResidentTreeNode k_node_flash_bytes_remaining = {
  TREE_ID_FLASH_BYTES_REMAINING, TREE_NODE_STATIC, 0, 0, 0, list_flash_stat_node,
  "Bytes Remaining (sector)", TREE_ACCESS_READ, 0, "Bytes remaining in the metadata flash sector", false, 0, 0};
static ResidentTreeNode k_node_flash_current_settings_phys_addr = {
  TREE_ID_FLASH_CURRENT_SETTINGS_PHYS_ADDR, TREE_NODE_STATIC, 0, 0, 0, list_flash_stat_node,
  "Current Settings Object Address", TREE_ACCESS_READ, 0,
  "Absolute flash base address of the active KV/settings record", false, 0, 0};
static ResidentTreeNode k_node_flash_current_settings_object_size = {
  TREE_ID_FLASH_CURRENT_SETTINGS_OBJECT_SIZE, TREE_NODE_STATIC, 0, 0, 0, list_flash_stat_node,
  "Current Settings Object Size", TREE_ACCESS_READ, 0,
  "KV blob record_total: logical byte length including trailing CRC", false, 0, 0};
static ResidentTreeNode k_node_debug_flash = {
  TREE_ID_DEBUG_FLASH, TREE_NODE_STATIC, 0, &k_node_flash_current_settings_slot, 0, list_static_node,
  "Flash", TREE_ACCESS_READ, 0, 0, false, 0, 0};
static ResidentTreeNode k_node_debug = {
  TREE_ID_DEBUG, TREE_NODE_STATIC, 0, &k_node_debug_flash, 0, list_static_node, "Debug",
  TREE_ACCESS_READ, 0, 0, false, 0, 0};
static ResidentTreeNode k_node_reboot = {
  TREE_ID_REBOOT, TREE_NODE_STATIC, 0, 0, execute_reboot_node, list_static_node, "Reboot",
  TREE_ACCESS_EXECUTE, 0, "Reset the device after replying", true, 0, 0};
static ResidentTreeNode g_app_root_node = {
  TREE_ID_APP, TREE_NODE_STATIC, 0, 0, 0, list_app_root_node, "App", TREE_ACCESS_READ, 0, 0, false, 0, 0};

static void rebuild_app_action_nodes(void);
static void rebuild_flash_kv_nodes(void);
static const char *program_state_text(ResidentProgramState state);

static ResidentTreeRootLayout resident_device_tree_root_layout(void)
{
  ResidentTreeRootLayout layout;
  memset(&layout, 0, sizeof(layout));
  layout.network = &k_node_network;
  layout.program = &k_node_program;
  layout.hardware = &k_node_hardware;
  layout.debug = &k_node_debug;
  layout.reboot = &k_node_reboot;
  layout.app_root = &g_app_root_node;
  layout.program_state = &k_node_program_state;
  layout.program_tcp_port = &k_node_program_tcp_port;
  layout.network_mac = &k_node_network_mac;
  layout.ipv4_address = &k_node_ipv4_address;
  layout.ipv4_subnet = &k_node_ipv4_subnet;
  layout.ipv4_gateway = &k_node_ipv4_gateway;
  layout.ipv4_dhcp = &k_node_ipv4_dhcp;
  layout.hardware_cpu_temp = &k_node_hardware_cpu_temp;
  layout.hardware_config = &k_node_hardware_config;
  layout.hardware_estop = &k_node_hardware_estop;
  layout.hardware_rail_a = &k_node_hardware_rail_a;
  layout.hardware_rail_b = &k_node_hardware_rail_b;
  layout.hardware_button = &k_node_hardware_button;
  layout.rail_mode = &k_node_rail_mode;
  layout.rail_output = &k_node_rail_output;
  layout.flash_current_settings_slot = &k_node_flash_current_settings_slot;
  layout.flash_bytes_used = &k_node_flash_bytes_used;
  layout.flash_bytes_remaining = &k_node_flash_bytes_remaining;
  layout.flash_current_settings_phys_addr = &k_node_flash_current_settings_phys_addr;
  layout.flash_current_settings_object_size = &k_node_flash_current_settings_object_size;
  layout.app_action_nodes = g_app_action_nodes;
  layout.app_action_count = g_app_mount.action_count;
  layout.flash_kv_nodes = g_flash_kv_nodes;
  layout.flash_kv_count = (boot_metadata_stored_kv_count() > BOOT_METADATA_STORED_KV_MAX)
                            ? BOOT_METADATA_STORED_KV_MAX : boot_metadata_stored_kv_count();
  layout.app_mounted = g_app_mount.mounted;
  return layout;
}

static void resident_device_tree_link_static_nodes(void)
{
  k_node_program_state.next = &k_node_program_tcp_port;
  k_node_program_tcp_port.next = &k_node_program_apply_boot;
  k_node_program_apply_boot.next = 0;

  const ResidentTreeRootLayout layout = resident_device_tree_root_layout();
  resident_tree_root_refresh(&layout);
}

static const ResidentTreeNode *resident_device_tree_root(void)
{
  const ResidentTreeRootLayout layout = resident_device_tree_root_layout();
  resident_tree_root_refresh(&layout);
  rebuild_app_action_nodes();
  rebuild_flash_kv_nodes();
  return resident_tree_root_first(&layout);
}

static void rebuild_app_action_nodes(void)
{
  resident_tree_app_rebuild_nodes(&g_app_mount, &g_app_root_node, g_app_action_nodes);
  for (uint8_t i = 0U; i < g_app_mount.action_count; i++)
  {
    g_app_action_nodes[i].execute = execute_app_action_node;
    g_app_action_nodes[i].list = list_app_action_node;
    g_app_action_nodes[i].access = TREE_ACCESS_EXECUTE;
  }
}

static void rebuild_flash_kv_nodes(void)
{
  memset(g_flash_kv_nodes, 0, sizeof(g_flash_kv_nodes));
  const uint16_t kv_count = boot_metadata_stored_kv_count();
  const uint16_t kv_limit = (kv_count > BOOT_METADATA_STORED_KV_MAX) ? BOOT_METADATA_STORED_KV_MAX : kv_count;

  k_node_flash_current_settings_object_size.next = (kv_limit != 0U) ? &g_flash_kv_nodes[0] : 0;
  for (uint16_t i = 0U; i < kv_limit; i++)
  {
    g_flash_kv_nodes[i].id = (uint8_t)(TREE_ID_FLASH_CURRENT_SETTINGS_OBJECT_SIZE + i + 1U);
    g_flash_kv_nodes[i].kind = TREE_NODE_FLASH_KV;
    g_flash_kv_nodes[i].child = 0;
    g_flash_kv_nodes[i].next = (i + 1U < kv_limit) ? &g_flash_kv_nodes[i + 1U] : 0;
    g_flash_kv_nodes[i].list = list_flash_kv_node;
    g_flash_kv_nodes[i].access = TREE_ACCESS_READ;
  }
}

static const ResidentTreeNode *resolve_location_node(const uint8_t *location, uint8_t depth)
{
  return resident_tree_resolve_location_node(resident_device_tree_root(), location, depth);
}

static bool metadata_read_bytes(uint32_t key, uint8_t *out, uint16_t expected_len)
{
  BootMetadataValueView value;
  if ((out == 0) || (boot_metadata_get(key, &value) != 0) || (value.value_len != expected_len))
  {
    return false;
  }
  memcpy(out, value.value, expected_len);
  return true;
}

static uint8_t metadata_read_u8(uint32_t key, uint8_t fallback)
{
  uint8_t value;
  return metadata_read_bytes(key, &value, sizeof(value)) ? value : fallback;
}

static int metadata_write_u32(uint32_t key, uint32_t value)
{
  uint8_t encoded[4];
  encoded[0] = (uint8_t)(value & 0xFFU);
  encoded[1] = (uint8_t)((value >> 8U) & 0xFFU);
  encoded[2] = (uint8_t)((value >> 16U) & 0xFFU);
  encoded[3] = (uint8_t)((value >> 24U) & 0xFFU);
  return boot_metadata_set(key, encoded, sizeof(encoded));
}

static void metadata_get_ipv4(uint8_t ip[4], uint8_t subnet[4], uint8_t gateway[4])
{
  (void)metadata_read_bytes(BOOT_KV_IPV4_ADDR, ip, 4U);
  (void)metadata_read_bytes(BOOT_KV_IPV4_SUBNET, subnet, 4U);
  (void)metadata_read_bytes(BOOT_KV_IPV4_GW, gateway, 4U);
}

static void format_mac(char *text, size_t text_size)
{
  uint8_t mac[6];
  (void)metadata_read_bytes(BOOT_KV_NET_MAC, mac, sizeof(mac));
  (void)snprintf(text, text_size, "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int append_u16(uint8_t *response, uint16_t response_max, uint16_t *offset, uint16_t value)
{
  return resident_tree_append_u16(response, response_max, offset, value);
}

static int append_bytes(uint8_t *response, uint16_t response_max, uint16_t *offset,
                        const void *data, uint16_t data_len)
{
  return resident_tree_append_bytes(response, response_max, offset, data, data_len);
}

static int append_list_item(uint8_t *response, uint16_t response_max, uint16_t *offset,
                            const ResidentTreeListItem *item)
{
  return resident_tree_append_list_item(response, response_max, offset, item);
}

static uint16_t list_item_encoded_len(const ResidentTreeListItem *item)
{
  return resident_tree_list_item_encoded_len(item);
}

static bool location_is_root(const uint8_t *location, uint8_t depth)
{
  (void)location;
  return depth == 0U;
}

static bool location_is_reboot(const uint8_t *location, uint8_t depth)
{
  return resolve_location_node(location, depth) == &k_node_reboot;
}

static bool location_is_program_state(const uint8_t *location, uint8_t depth)
{
  return resolve_location_node(location, depth) == &k_node_program_state;
}

static bool location_is_program_tcp_port(const uint8_t *location, uint8_t depth)
{
  return resolve_location_node(location, depth) == &k_node_program_tcp_port;
}

static bool location_is_program_apply_boot(const uint8_t *location, uint8_t depth)
{
  return resolve_location_node(location, depth) == &k_node_program_apply_boot;
}

static bool location_is_app_action(const uint8_t *location, uint8_t depth)
{
  const ResidentTreeNode *node = resolve_location_node(location, depth);
  return (node != 0) && (node->kind == TREE_NODE_APP_ACTION);
}

static bool location_is_cpu_temp(const uint8_t *location, uint8_t depth)
{
  return resolve_location_node(location, depth) == &k_node_hardware_cpu_temp;
}

static bool location_is_hardware_config(const uint8_t *location, uint8_t depth)
{
  return resolve_location_node(location, depth) == &k_node_hardware_config;
}

static bool location_is_hardware_poll_period(const uint8_t *location, uint8_t depth)
{
  return resolve_location_node(location, depth) == &k_node_hardware_poll_period;
}

static bool location_is_estop(const uint8_t *location, uint8_t depth)
{
  return resolve_location_node(location, depth) == &k_node_hardware_estop;
}

static bool location_is_button(const uint8_t *location, uint8_t depth)
{
  return resolve_location_node(location, depth) == &k_node_hardware_button;
}

static bool location_is_rail(const uint8_t *location, uint8_t depth)
{
  const ResidentTreeNode *node = resolve_location_node(location, depth);
  return (node == &k_node_hardware_rail_a) || (node == &k_node_hardware_rail_b);
}

static bool location_is_rail_mode(const uint8_t *location, uint8_t depth)
{
  return resolve_location_node(location, depth) == &k_node_rail_mode;
}

static bool location_is_rail_output(const uint8_t *location, uint8_t depth)
{
  return resolve_location_node(location, depth) == &k_node_rail_output;
}

static ResidentHardwareRailId rail_id_for_location(const uint8_t *location)
{
  return (location[1] == TREE_ID_HARDWARE_RAIL_B) ? RESIDENT_HARDWARE_RAIL_B : RESIDENT_HARDWARE_RAIL_A;
}

static uint16_t flash_kv_index_for_node(const ResidentTreeNode *node)
{
  if ((node == 0) || (node->kind != TREE_NODE_FLASH_KV))
  {
    return 0U;
  }

  return (uint16_t)(node->id - TREE_ID_FLASH_CURRENT_SETTINGS_OBJECT_SIZE);
}

static bool append_node_list_item(ResidentTreeListItem *items, uint16_t *count,
                                  const ResidentTreeNode *node, const uint8_t *parent_location)
{
  if ((items == 0) || (count == 0) || (node == 0))
  {
    return false;
  }

  if (node->list == 0)
  {
    return false;
  }

  ResidentTreeListItem item;
  memset(&item, 0, sizeof(item));
  if (!node->list(node, parent_location, &item))
  {
    return false;
  }
  items[(*count)++] = item;
  return true;
}

static void populate_static_list_item(const ResidentTreeNode *node, ResidentTreeListItem *item)
{
  resident_tree_populate_static_list_item(node, item);
}

static bool list_static_node(const ResidentTreeNode *node, const uint8_t *parent_location,
                             ResidentTreeListItem *item)
{
  (void)parent_location;

  if ((node == 0) || (item == 0) || (node->name == 0))
  {
    return false;
  }

  populate_static_list_item(node, item);
  return true;
}

static bool list_mac_node(const ResidentTreeNode *node, const uint8_t *parent_location,
                          ResidentTreeListItem *item)
{
  return resident_tree_network_list_mac(node, parent_location, item);
}

static bool list_dhcp_node(const ResidentTreeNode *node, const uint8_t *parent_location,
                           ResidentTreeListItem *item)
{
  return resident_tree_network_list_dhcp(node, parent_location, item);
}

static bool list_ipv4_node(const ResidentTreeNode *node, const uint8_t *parent_location,
                           ResidentTreeListItem *item)
{
  return resident_tree_network_list_ipv4(node, parent_location, item);
}

static bool list_hardware_text_node(const ResidentTreeNode *node, const uint8_t *parent_location,
                                    ResidentTreeListItem *item)
{
  if (node == &k_node_hardware_cpu_temp)
  {
    return resident_tree_hardware_list_cpu_temp(node, parent_location, item);
  }
  if (node == &k_node_hardware_estop)
  {
    return resident_tree_hardware_list_estop(node, parent_location, item);
  }
  if (node == &k_node_hardware_button)
  {
    return resident_tree_hardware_list_button(node, parent_location, item);
  }
  if (node == &k_node_hardware_poll_period)
  {
    return resident_tree_hardware_list_poll_period(node, parent_location, item);
  }
  return false;
}

static bool list_rail_text_node(const ResidentTreeNode *node, const uint8_t *parent_location,
                                ResidentTreeListItem *item)
{
  if (node == &k_node_rail_mode)
  {
    return resident_tree_hardware_list_rail_mode(node, parent_location, item);
  }
  if (node == &k_node_rail_output)
  {
    return resident_tree_hardware_list_rail_output(node, parent_location, item);
  }
  return false;
}

static bool list_flash_stat_node(const ResidentTreeNode *node, const uint8_t *parent_location,
                                 ResidentTreeListItem *item)
{
  if (node == &k_node_flash_current_settings_slot)
  {
    return resident_tree_debug_list_flash_current_settings_slot(node, parent_location, item);
  }
  if (node == &k_node_flash_bytes_used)
  {
    return resident_tree_debug_list_flash_bytes_used(node, parent_location, item);
  }
  if (node == &k_node_flash_bytes_remaining)
  {
    return resident_tree_debug_list_flash_bytes_remaining(node, parent_location, item);
  }
  if (node == &k_node_flash_current_settings_phys_addr)
  {
    return resident_tree_debug_list_flash_current_settings_phys_addr(node, parent_location, item);
  }
  if (node == &k_node_flash_current_settings_object_size)
  {
    return resident_tree_debug_list_flash_current_settings_object_size(node, parent_location, item);
  }
  return false;
}

static bool list_app_root_node(const ResidentTreeNode *node, const uint8_t *parent_location,
                               ResidentTreeListItem *item)
{
  return list_static_node(node, parent_location, item);
}

static bool list_app_action_node(const ResidentTreeNode *node, const uint8_t *parent_location,
                                 ResidentTreeListItem *item)
{
  return resident_tree_app_list_action(&g_app_mount, node, parent_location, item);
}

static bool list_flash_kv_node(const ResidentTreeNode *node, const uint8_t *parent_location,
                               ResidentTreeListItem *item)
{
  return resident_tree_debug_list_flash_kv(node, parent_location, item);
}

static const char *program_state_text(ResidentProgramState state)
{
  switch (state)
  {
    case RESIDENT_PROGRAM_STATE_ERASING:
      return "Erasing";
    case RESIDENT_PROGRAM_STATE_PROGRAMMING_READY:
      return "ProgrammingReady";
    case RESIDENT_PROGRAM_STATE_STOPPED:
      return "Stopped";
    case RESIDENT_PROGRAM_STATE_PAUSED:
      return "Paused";
    case RESIDENT_PROGRAM_STATE_RUNNING:
      return "Running";
    default:
      return "Stopped";
  }
}

static bool parse_program_state(const uint8_t *value, uint16_t value_len, ResidentProgramState *state)
{
  if ((value == 0) || (state == 0))
  {
    return false;
  }

  const struct
  {
    const char *name;
    ResidentProgramState state;
  } states[] = {
    {"Erasing", RESIDENT_PROGRAM_STATE_ERASING},
    {"ProgrammingReady", RESIDENT_PROGRAM_STATE_PROGRAMMING_READY},
    {"Stopped", RESIDENT_PROGRAM_STATE_STOPPED},
    {"Paused", RESIDENT_PROGRAM_STATE_PAUSED},
    {"Running", RESIDENT_PROGRAM_STATE_RUNNING},
  };

  for (uint8_t i = 0U; i < (uint8_t)(sizeof(states) / sizeof(states[0])); i++)
  {
    const size_t name_len = strlen(states[i].name);
    if ((value_len == name_len) && (memcmp(value, states[i].name, name_len) == 0))
    {
      *state = states[i].state;
      return true;
    }
  }

  return false;
}

static bool list_program_node(const ResidentTreeNode *node, const uint8_t *parent_location,
                              ResidentTreeListItem *item)
{
  (void)parent_location;

  if ((node == 0) || (item == 0))
  {
    return false;
  }

  populate_static_list_item(node, item);
  if (node == &k_node_program_state)
  {
    item->value = program_state_text(resident_program_manager_state());
  }
  else if (node == &k_node_program_tcp_port)
  {
    (void)snprintf(g_list_value_text[0], sizeof(g_list_value_text[0]), "%d",
                   resident_program_manager_tcp_port());
    item->value = g_list_value_text[0];
  }
  return true;
}

static bool location_is_ipv4_leaf(const uint8_t *location, uint8_t depth)
{
  const ResidentTreeNode *node = resolve_location_node(location, depth);
  return (node == &k_node_ipv4_address) || (node == &k_node_ipv4_subnet) ||
         (node == &k_node_ipv4_gateway);
}

static bool location_is_ipv4_dhcp(const uint8_t *location, uint8_t depth)
{
  return resolve_location_node(location, depth) == &k_node_ipv4_dhcp;
}

static bool location_is_mac(const uint8_t *location, uint8_t depth)
{
  return resolve_location_node(location, depth) == &k_node_network_mac;
}

static bool location_is_debug_flash(const uint8_t *location, uint8_t depth)
{
  return resolve_location_node(location, depth) == &k_node_debug_flash;
}

static bool location_is_flash_current_settings_slot(const uint8_t *location, uint8_t depth)
{
  return resolve_location_node(location, depth) == &k_node_flash_current_settings_slot;
}

static bool location_is_flash_bytes_used(const uint8_t *location, uint8_t depth)
{
  return resolve_location_node(location, depth) == &k_node_flash_bytes_used;
}

static bool location_is_flash_bytes_remaining(const uint8_t *location, uint8_t depth)
{
  return resolve_location_node(location, depth) == &k_node_flash_bytes_remaining;
}

static bool location_is_flash_current_settings_phys_addr(const uint8_t *location, uint8_t depth)
{
  return resolve_location_node(location, depth) == &k_node_flash_current_settings_phys_addr;
}

static bool location_is_flash_current_settings_object_size(const uint8_t *location, uint8_t depth)
{
  return resolve_location_node(location, depth) == &k_node_flash_current_settings_object_size;
}

static bool location_is_flash_stored_kv(const uint8_t *location, uint8_t depth)
{
  const ResidentTreeNode *node = resolve_location_node(location, depth);
  return (node != 0) && (node->kind == TREE_NODE_FLASH_KV);
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

static int execute_reboot_node(const ResidentTreeNode *node, const uint8_t *location,
                               uint8_t depth, const uint8_t *args, uint16_t args_len,
                          uint8_t *response, uint16_t response_max, uint16_t *response_len)
{
  (void)node;
  (void)location;
  (void)depth;
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

static int execute_app_action_node(const ResidentTreeNode *node, const uint8_t *location,
                                   uint8_t depth, const uint8_t *args, uint16_t args_len,
                                   uint8_t *response, uint16_t response_max, uint16_t *response_len)
{
  return resident_tree_app_execute_action(&g_app_mount, node, location, depth, args, args_len,
                                          response, response_max, response_len);
}

static int execute_program_apply_boot_node(const ResidentTreeNode *node, const uint8_t *location,
                                           uint8_t depth, const uint8_t *args, uint16_t args_len,
                                           uint8_t *response, uint16_t response_max, uint16_t *response_len)
{
  (void)node;
  (void)location;
  (void)depth;
  (void)args;
  (void)args_len;
  (void)response;
  (void)response_max;
  if (response_len == 0)
  {
    return PROTO_RESULT_PARSE;
  }

  *response_len = 0U;
  return (resident_program_manager_request_apply_boot_update() == 0) ? PROTO_RESULT_OK : PROTO_RESULT_BUSY;
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
  resident_tree_app_clear(&g_app_mount);
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

  if (resident_tree_app_mount_name(&g_app_mount, name) != 0)
  {
    return -1;
  }
  *mount = &g_app_mount;
  return 0;
}

void resident_device_tree_unmount_app(AppDeviceTreeMount mount)
{
  if (mount == &g_app_mount)
  {
    resident_tree_app_unmount(&g_app_mount);
  }
}

void resident_device_tree_unmount_all_app(void)
{
  resident_tree_app_unmount(&g_app_mount);
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
  if (mount != &g_app_mount)
  {
    return -1;
  }

  return resident_tree_app_register_action(&g_app_mount, path, callback, context);
}

int resident_device_tree_list(const uint8_t *location, uint8_t depth, uint8_t start_after,
                              uint8_t *response, uint16_t response_max, uint16_t *response_len,
                              bool *has_more)
{
  uint16_t offset = 0U;
  ResidentTreeListItem *items = g_list_items;
  uint16_t count = 0U;
  uint16_t emitted_count = 0U;
  bool cursor_open = false;

  if ((response == 0) || (response_len == 0) || (has_more == 0) ||
      ((depth != 0U) && (location == 0)))
  {
    return PROTO_RESULT_PARSE;
  }

  *has_more = false;

  const ResidentTreeNode *parent = (depth == 0U) ? 0 : resolve_location_node(location, depth);
  const ResidentTreeNode *child = (depth == 0U) ? resident_device_tree_root() : ((parent != 0) ? parent->child : 0);
  if ((depth != 0U) && (parent == 0))
  {
    return PROTO_RESULT_NOT_FOUND;
  }
  if ((depth != 0U) && (child == 0) && (parent != &g_app_root_node))
  {
    return PROTO_RESULT_NOT_FOUND;
  }

  for (const ResidentTreeNode *node = child; node != 0; node = node->next)
  {
    if (!append_node_list_item(items, &count, node, location))
    {
      return PROTO_RESULT_GENERIC;
    }
  }

  if (append_u16(response, response_max, &offset, 0U) != PROTO_RESULT_OK)
  {
    return PROTO_RESULT_GENERIC;
  }

  for (uint16_t i = 0U; i < count; i++)
  {
    if (!cursor_open)
    {
      if (start_after == 0U)
      {
        cursor_open = true;
      }
      else if (items[i].id == start_after)
      {
        cursor_open = true;
        continue;
  }
  else
  {
        continue;
      }
  }

    const uint16_t encoded_len = list_item_encoded_len(&items[i]);
    if ((encoded_len == 0U) || ((offset + encoded_len) > response_max))
  {
      *has_more = true;
      break;
  }

    const int result = append_list_item(response, response_max, &offset, &items[i]);
    if (result != PROTO_RESULT_OK)
    {
      return result;
    }
    emitted_count++;
  }

  memcpy(response, &emitted_count, sizeof(emitted_count));
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
  uint8_t raw_kv_value[BOOT_METADATA_KV_VALUE_MAX];

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
                   (metadata_read_u8(BOOT_KV_NET_DHCP, 0U) != 0U) ? "Enabled" : "Disabled");
  }
  else if (location_is_ipv4_leaf(location, depth))
  {
    if (metadata_read_u8(BOOT_KV_NET_DHCP, 0U) != 0U)
    {
      (void)resident_network_get_ipv4(ip, subnet, gateway);
    }
    else
    {
      metadata_get_ipv4(ip, subnet, gateway);
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
    resident_text_format_ipv4(selected, value, sizeof(value));
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
  else if (location_is_flash_stored_kv(location, depth))
  {
    const uint16_t kv_index = (uint16_t)(location[2] - TREE_ID_FLASH_CURRENT_SETTINGS_OBJECT_SIZE);
    uint32_t raw_key;
    uint16_t raw_len;
    if (boot_metadata_read_stored_kv_raw(kv_index, &raw_key, raw_kv_value, sizeof(raw_kv_value), &raw_len) != 0)
    {
      return PROTO_RESULT_NOT_FOUND;
    }
    (void)raw_key;
    resident_text_bytes_to_hex(raw_kv_value, raw_len, value, sizeof(value));
  }
  else if (location_is_program_state(location, depth))
  {
    (void)snprintf(value, sizeof(value), "%s", program_state_text(resident_program_manager_state()));
  }
  else if (location_is_program_tcp_port(location, depth))
  {
    (void)snprintf(value, sizeof(value), "%d", resident_program_manager_tcp_port());
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
    if (!resident_text_parse_mac(value, value_len, parsed_mac))
    {
      return PROTO_RESULT_INVALID_VALUE;
    }

    if ((boot_metadata_set(BOOT_KV_NET_MAC, parsed_mac, sizeof(parsed_mac)) != 0) ||
        (boot_metadata_save_to_flash() != 0))
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
    if (!resident_text_parse_net_dhcp(value, value_len, &parsed_dhcp_enabled))
    {
      return PROTO_RESULT_INVALID_VALUE;
    }

    const uint8_t cur = metadata_read_u8(BOOT_KV_NET_DHCP, 0U);
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

    if ((boot_metadata_set(BOOT_KV_NET_DHCP, &parsed_dhcp_enabled, sizeof(parsed_dhcp_enabled)) != 0) ||
        (boot_metadata_save_to_flash() != 0))
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
    if (!resident_text_parse_u32(value, value_len, &parsed_u32))
    {
      return PROTO_RESULT_INVALID_VALUE;
    }

    if ((metadata_write_u32(BOOT_KV_HW_POLL_MS, parsed_u32) != 0) ||
        (boot_metadata_save_to_flash() != 0))
    {
      return PROTO_RESULT_GENERIC;
    }

    (void)resident_hardware_set_poll_period_ms(parsed_u32);
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
    if (!resident_text_parse_rail_mode(value, value_len, &parsed_mode))
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
    if (location_is_program_state(location, depth))
    {
      ResidentProgramState requested_state;
      if (!parse_program_state(value, value_len, &requested_state))
      {
        return PROTO_RESULT_INVALID_VALUE;
      }

      if (resident_program_manager_request_state(requested_state) != 0)
      {
        return PROTO_RESULT_BUSY;
      }
      if (response_max < 1U)
      {
        return PROTO_RESULT_GENERIC;
      }
      response[0] = 0U;
      *response_len = 1U;
      return PROTO_RESULT_OK;
    }

    if (location_is_reboot(location, depth) || location_is_app_action(location, depth) ||
        location_is_cpu_temp(location, depth) || location_is_estop(location, depth) ||
        location_is_button(location, depth) || location_is_rail_output(location, depth) ||
        location_is_flash_current_settings_slot(location, depth) ||
        location_is_flash_bytes_used(location, depth) ||
        location_is_flash_bytes_remaining(location, depth) ||
        location_is_flash_current_settings_phys_addr(location, depth) ||
        location_is_flash_current_settings_object_size(location, depth) ||
        location_is_flash_stored_kv(location, depth) ||
        location_is_program_tcp_port(location, depth))
    {
      return PROTO_RESULT_INVALID_VALUE;
    }
    return PROTO_RESULT_NOT_FOUND;
  }

  if (metadata_read_u8(BOOT_KV_NET_DHCP, 0U) != 0U)
  {
    return PROTO_RESULT_INVALID_VALUE;
  }

  if (!resident_text_parse_ipv4(value, value_len, parsed_ip))
  {
    return PROTO_RESULT_INVALID_VALUE;
  }

  if (resident_text_ipv4_is_unusable_reserved(parsed_ip))
  {
    return PROTO_RESULT_INVALID_VALUE;
  }

  metadata_get_ipv4(ip, subnet, gateway);
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

  if ((boot_metadata_set(BOOT_KV_IPV4_ADDR, ip, sizeof(ip)) != 0) ||
      (boot_metadata_set(BOOT_KV_IPV4_SUBNET, subnet, sizeof(subnet)) != 0) ||
      (boot_metadata_set(BOOT_KV_IPV4_GW, gateway, sizeof(gateway)) != 0) ||
      (boot_metadata_save_to_flash() != 0))
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
  if ((location == 0) || (response == 0) || (response_len == 0) ||
      ((args_len != 0U) && (args == 0)))
  {
    return PROTO_RESULT_PARSE;
  }

  const ResidentTreeNode *node = resolve_location_node(location, depth);
  if ((node != 0) && (node->execute != 0))
  {
    return node->execute(node, location, depth, args, args_len, response, response_max, response_len);
  }

  if (location_is_mac(location, depth) || location_is_ipv4_leaf(location, depth) ||
      location_is_ipv4_dhcp(location, depth) ||
      location_is_cpu_temp(location, depth) || location_is_hardware_poll_period(location, depth) ||
      location_is_estop(location, depth) || location_is_button(location, depth) ||
      location_is_rail_mode(location, depth) || location_is_rail_output(location, depth) ||
      location_is_program_state(location, depth) || location_is_program_tcp_port(location, depth) ||
      location_is_program_apply_boot(location, depth) ||
      location_is_flash_current_settings_slot(location, depth) ||
      location_is_flash_bytes_used(location, depth) || location_is_flash_bytes_remaining(location, depth) ||
      location_is_flash_current_settings_phys_addr(location, depth) ||
      location_is_flash_current_settings_object_size(location, depth) ||
      location_is_flash_stored_kv(location, depth))
  {
    return PROTO_RESULT_INVALID_VALUE;
  }

  return PROTO_RESULT_NOT_FOUND;
}
