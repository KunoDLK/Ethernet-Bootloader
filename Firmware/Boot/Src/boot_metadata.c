#include "boot_metadata.h"

#include "lwipopts.h"
#include "stm32f4xx_hal.h"

#include <string.h>

static BootMetadata g_metadata;
static uint32_t g_active_settings_slot;

static uint32_t metadata_crc32_words(const uint32_t *words, uint32_t word_count)
{
  uint32_t crc = 0xFFFFFFFFUL;

  for (uint32_t i = 0U; i < word_count; i++)
  {
    crc ^= words[i];
    for (uint32_t bit = 0U; bit < 32U; bit++)
    {
      crc = (crc >> 1U) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
    }
  }

  return ~crc;
}

static uint32_t metadata_crc32(const BootMetadata *metadata)
{
  return metadata_crc32_words((const uint32_t *)metadata,
                              (sizeof(BootMetadata) / sizeof(uint32_t)) - 1U);
}

static bool metadata_is_valid(const BootMetadata *metadata)
{
  return (metadata->magic == BOOT_METADATA_MAGIC) &&
         (metadata->version == BOOT_METADATA_VERSION) &&
         (metadata->crc32 == metadata_crc32(metadata));
}

static uint32_t metadata_bitmap_byte_len(void)
{
  return BOOT_METADATA_BITMAP_BYTES_FOR_SLOTS(BOOT_METADATA_MAX_SLOTS);
}

static uint32_t metadata_slot_address(uint32_t slot_index)
{
  return BOOT_METADATA_BASE_ADDR + metadata_bitmap_byte_len() +
         (slot_index * BOOT_METADATA_SLOT_STRIDE_BYTES);
}

static uint32_t metadata_count_prefix_zeros(void)
{
  const uint32_t limit = BOOT_METADATA_MAX_SLOTS;

  for (uint32_t i = 0U; i < limit; i++)
  {
    uint32_t byte_idx = i / 8U;
    uint32_t bit_in_byte = i % 8U;
    uint8_t byte = *(const volatile uint8_t *)(BOOT_METADATA_BASE_ADDR + byte_idx);
    if (((uint32_t)(byte >> bit_in_byte) & 1U) != 0U)
    {
      return i;
    }
  }

  return limit;
}

static int metadata_program_word(uint32_t address, uint32_t value)
{
  HAL_StatusTypeDef st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address, value);
  return (st == HAL_OK) ? 0 : -1;
}

static int metadata_program_run_of_words(uint32_t address, const uint32_t *words, uint32_t word_count)
{
  for (uint32_t i = 0U; i < word_count; i++)
  {
    if (metadata_program_word(address + (i * sizeof(uint32_t)), words[i]) != 0)
    {
      return -1;
    }
  }

  return 0;
}

static int metadata_program_bitmap_bit(uint32_t bit_index)
{
  uint32_t byte_pos = bit_index / 8U;
  uint32_t bit_in_byte = bit_index % 8U;
  uint32_t word_addr = BOOT_METADATA_BASE_ADDR + (byte_pos & ~3U);
  uint32_t shift = ((byte_pos % 4U) * 8U) + bit_in_byte;
  uint32_t old = *(const volatile uint32_t *)word_addr;
  uint32_t mask = 1UL << shift;

  if ((old & mask) == 0U)
  {
    return -1;
  }

  uint32_t value = old & ~mask;
  if (value == old)
  {
    return -1;
  }

  return metadata_program_word(word_addr, value);
}

static int metadata_erase_sector(void)
{
  FLASH_EraseInitTypeDef erase = {0};
  uint32_t sector_error = 0U;

  erase.TypeErase = FLASH_TYPEERASE_SECTORS;
  erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
  erase.Sector = FLASH_SECTOR_11;
  erase.NbSectors = 1U;

  return (HAL_FLASHEx_Erase(&erase, &sector_error) == HAL_OK) ? 0 : -1;
}

static int metadata_append_slot_unlocked(const BootMetadata *record)
{
  uint32_t k = metadata_count_prefix_zeros();
  uint32_t slot_dst;

  if (k >= BOOT_METADATA_MAX_SLOTS)
  {
    if (metadata_erase_sector() != 0)
    {
      return -1;
    }
    k = 0U;
  }

  slot_dst = metadata_slot_address(k);

  if (metadata_program_run_of_words(slot_dst, (const uint32_t *)record,
                                   (uint32_t)(sizeof(BootMetadata) / sizeof(uint32_t))) != 0)
  {
    return -1;
  }

  if (metadata_program_bitmap_bit(k) != 0)
  {
    return -1;
  }

  g_active_settings_slot = k;
  return 0;
}

static void metadata_set_defaults(void)
{
  memset(&g_metadata, 0, sizeof(g_metadata));
  g_metadata.magic = BOOT_METADATA_MAGIC;
  g_metadata.version = BOOT_METADATA_VERSION;
  g_metadata.net_ipv4_addr[0] = RESIDENT_IPV4_ADDR0;
  g_metadata.net_ipv4_addr[1] = RESIDENT_IPV4_ADDR1;
  g_metadata.net_ipv4_addr[2] = RESIDENT_IPV4_ADDR2;
  g_metadata.net_ipv4_addr[3] = RESIDENT_IPV4_ADDR3;
  g_metadata.net_ipv4_subnet[0] = RESIDENT_IPV4_MASK0;
  g_metadata.net_ipv4_subnet[1] = RESIDENT_IPV4_MASK1;
  g_metadata.net_ipv4_subnet[2] = RESIDENT_IPV4_MASK2;
  g_metadata.net_ipv4_subnet[3] = RESIDENT_IPV4_MASK3;
  g_metadata.net_ipv4_gateway[0] = RESIDENT_IPV4_GW0;
  g_metadata.net_ipv4_gateway[1] = RESIDENT_IPV4_GW1;
  g_metadata.net_ipv4_gateway[2] = RESIDENT_IPV4_GW2;
  g_metadata.net_ipv4_gateway[3] = RESIDENT_IPV4_GW3;
  g_metadata.net_mac[0] = 0x00U;
  g_metadata.net_mac[1] = 0x80U;
  g_metadata.net_mac[2] = 0xE1U;
  g_metadata.net_mac[3] = 0x00U;
  g_metadata.net_mac[4] = 0x00U;
  g_metadata.net_mac[5] = 0x00U;
  g_metadata.hardware_poll_period_ms = BOOT_METADATA_DEFAULT_HARDWARE_POLL_PERIOD_MS;
  g_metadata.rail_a_mode = BOOT_METADATA_DEFAULT_RAIL_MODE;
  g_metadata.rail_b_mode = BOOT_METADATA_DEFAULT_RAIL_MODE;
  g_metadata.reserved_rails[0] = 0U;
  g_metadata.reserved_rails[1] = 0U;
  g_metadata.crc32 = metadata_crc32(&g_metadata);
}

static int metadata_commit(void)
{
  BootMetadata next = g_metadata;
  next.sequence++;
  next.crc32 = metadata_crc32(&next);

  HAL_StatusTypeDef status = HAL_FLASH_Unlock();
  if (status != HAL_OK)
  {
    return -1;
  }

  int result = metadata_append_slot_unlocked(&next);

  (void)HAL_FLASH_Lock();

  if (result == 0)
  {
    g_metadata = next;
  }

  return result;
}

void boot_metadata_init(void)
{
  const BootMetadata *flat = (const BootMetadata *)(uintptr_t)BOOT_METADATA_BASE_ADDR;

  /*
   * Legacy: older firmware stored one BootMetadata at the sector base with no bitmap.
   * Interpreting those bytes as a bitmap makes prefix-zero count wrong (e.g. jump to slot 6).
   * Migrate by erasing and writing the saved record as slot 0.
   */
  if (metadata_is_valid(flat))
  {
    const BootMetadata saved = *flat;

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
      return;
    }

    if (metadata_erase_sector() != 0)
    {
      (void)HAL_FLASH_Lock();
      return;
    }

    if (metadata_append_slot_unlocked(&saved) != 0)
    {
      (void)HAL_FLASH_Lock();
      return;
    }

    (void)HAL_FLASH_Lock();
    g_metadata = saved;
    return;
  }

  uint32_t k = metadata_count_prefix_zeros();

  if (k == 0U)
  {
    metadata_set_defaults();
    (void)metadata_commit();
    return;
  }

  uint32_t newest = k - 1U;
  const BootMetadata *stored =
      (const BootMetadata *)(uintptr_t)metadata_slot_address(newest);

  if (metadata_is_valid(stored))
  {
    g_metadata = *stored;
    g_active_settings_slot = newest;
  }
  else
  {
    /*
     * Bitmap and slot stream out of sync (e.g. power loss mid-commit) or other garbage:
     * erase and start from defaults in slot layout.
     */
    metadata_set_defaults();

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
      return;
    }

    if (metadata_erase_sector() != 0)
    {
      (void)HAL_FLASH_Lock();
      return;
    }

    if (metadata_append_slot_unlocked(&g_metadata) != 0)
    {
      (void)HAL_FLASH_Lock();
      return;
    }

    (void)HAL_FLASH_Lock();
  }
}

const BootMetadata *boot_metadata_get(void)
{
  return &g_metadata;
}

bool boot_metadata_app_is_enabled(void)
{
  return g_metadata.app_disabled == 0U;
}

bool boot_metadata_app_is_valid(void)
{
  return g_metadata.app_valid != 0U;
}

int boot_metadata_set_app_valid(uint32_t app_version)
{
  g_metadata.app_valid = 1U;
  g_metadata.app_disabled = 0U;
  g_metadata.app_version = app_version;
  g_metadata.last_fault_reason = BOOT_APP_FAULT_NONE;
  g_metadata.last_fault_pc = 0U;
  g_metadata.last_fault_lr = 0U;
  return metadata_commit();
}

int boot_metadata_disable_app(BootAppFaultReason reason, uint32_t pc, uint32_t lr)
{
  g_metadata.app_disabled = 1U;
  g_metadata.last_fault_reason = (uint32_t)reason;
  g_metadata.last_fault_pc = pc;
  g_metadata.last_fault_lr = lr;
  return metadata_commit();
}

int boot_metadata_enable_app(void)
{
  g_metadata.app_disabled = 0U;
  g_metadata.last_fault_reason = BOOT_APP_FAULT_NONE;
  g_metadata.last_fault_pc = 0U;
  g_metadata.last_fault_lr = 0U;
  return metadata_commit();
}

void boot_metadata_get_ipv4(uint8_t ip[4], uint8_t subnet[4], uint8_t gateway[4])
{
  if (ip != 0)
  {
    memcpy(ip, g_metadata.net_ipv4_addr, 4U);
  }

  if (subnet != 0)
  {
    memcpy(subnet, g_metadata.net_ipv4_subnet, 4U);
  }

  if (gateway != 0)
  {
    memcpy(gateway, g_metadata.net_ipv4_gateway, 4U);
  }
}

int boot_metadata_set_ipv4(const uint8_t ip[4], const uint8_t subnet[4], const uint8_t gateway[4])
{
  BootMetadata previous;

  if ((ip == 0) || (subnet == 0) || (gateway == 0))
  {
    return -1;
  }

  if ((memcmp(g_metadata.net_ipv4_addr, ip, 4U) == 0) &&
      (memcmp(g_metadata.net_ipv4_subnet, subnet, 4U) == 0) &&
      (memcmp(g_metadata.net_ipv4_gateway, gateway, 4U) == 0))
  {
    return 0;
  }

  previous = g_metadata;
  memcpy(g_metadata.net_ipv4_addr, ip, 4U);
  memcpy(g_metadata.net_ipv4_subnet, subnet, 4U);
  memcpy(g_metadata.net_ipv4_gateway, gateway, 4U);
  if (metadata_commit() != 0)
  {
    g_metadata = previous;
    return -1;
  }

  return 0;
}

void boot_metadata_get_mac(uint8_t mac[6])
{
  if (mac != 0)
  {
    memcpy(mac, g_metadata.net_mac, 6U);
  }
}

int boot_metadata_set_mac(const uint8_t mac[6])
{
  BootMetadata previous;

  if (mac == 0)
  {
    return -1;
  }

  if (memcmp(g_metadata.net_mac, mac, 6U) == 0)
  {
    return 0;
  }

  previous = g_metadata;
  memcpy(g_metadata.net_mac, mac, 6U);
  if (metadata_commit() != 0)
  {
    g_metadata = previous;
    return -1;
  }

  return 0;
}

uint32_t boot_metadata_get_hardware_poll_period_ms(void)
{
  if (g_metadata.hardware_poll_period_ms < BOOT_METADATA_DEFAULT_HARDWARE_POLL_PERIOD_MS)
  {
    return BOOT_METADATA_DEFAULT_HARDWARE_POLL_PERIOD_MS;
  }

  return g_metadata.hardware_poll_period_ms;
}

int boot_metadata_set_hardware_poll_period_ms(uint32_t period_ms)
{
  BootMetadata previous;

  if (period_ms < BOOT_METADATA_DEFAULT_HARDWARE_POLL_PERIOD_MS)
  {
    period_ms = BOOT_METADATA_DEFAULT_HARDWARE_POLL_PERIOD_MS;
  }

  if (g_metadata.hardware_poll_period_ms == period_ms)
  {
    return 0;
  }

  previous = g_metadata;
  g_metadata.hardware_poll_period_ms = period_ms;
  if (metadata_commit() != 0)
  {
    g_metadata = previous;
    return -1;
  }

  return 0;
}

static uint8_t rail_mode_clamp(uint8_t mode)
{
  return (mode > 2U) ? BOOT_METADATA_DEFAULT_RAIL_MODE : mode;
}

uint8_t boot_metadata_get_rail_a_mode(void)
{
  return rail_mode_clamp(g_metadata.rail_a_mode);
}

uint8_t boot_metadata_get_rail_b_mode(void)
{
  return rail_mode_clamp(g_metadata.rail_b_mode);
}

int boot_metadata_set_rail_a_mode(uint8_t mode)
{
  BootMetadata previous;

  if (mode > 2U)
  {
    return -1;
  }

  if (g_metadata.rail_a_mode == mode)
  {
    return 0;
  }

  previous = g_metadata;
  g_metadata.rail_a_mode = mode;
  if (metadata_commit() != 0)
  {
    g_metadata = previous;
    return -1;
  }

  return 0;
}

int boot_metadata_set_rail_b_mode(uint8_t mode)
{
  BootMetadata previous;

  if (mode > 2U)
  {
    return -1;
  }

  if (g_metadata.rail_b_mode == mode)
  {
    return 0;
  }

  previous = g_metadata;
  g_metadata.rail_b_mode = mode;
  if (metadata_commit() != 0)
  {
    g_metadata = previous;
    return -1;
  }

  return 0;
}

uint32_t boot_metadata_get_current_settings_slot(void)
{
  return g_active_settings_slot;
}

uint32_t boot_metadata_get_total_settings_slots(void)
{
  return BOOT_METADATA_MAX_SLOTS;
}
