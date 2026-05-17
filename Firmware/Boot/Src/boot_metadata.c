#include "boot_metadata.h"

#include "stm32f4xx_hal.h"

#include <string.h>

#define BOOT_SETTINGS_SCRATCH_MAX       (4096U)
#define BOOT_SETTINGS_ALIGNMENT         (8U)
#define BOOT_KV_ENTRY_HEADER_BYTES      (sizeof(uint32_t) + sizeof(uint8_t))
#define BOOT_KV_RECORD_HEADER_BYTES     (sizeof(uint32_t) * 4U)
#define BOOT_KV_RECORD_CRC_BYTES        (sizeof(uint32_t))

typedef struct
{
  uint32_t key;
  uint16_t value_len;
  uint8_t value[BOOT_METADATA_KV_VALUE_MAX];
  bool present;
} BootMetadataRamEntry;

static BootMetadataRamEntry g_entries[BOOT_METADATA_STORED_KV_MAX];
static uint16_t g_entry_count;
static bool g_store_dirty;
static bool g_store_loaded;
static bool g_entries_materialized;
static uint32_t g_active_slot;
static uint32_t g_active_settings_phys_addr;
static uint32_t g_active_settings_blob_size_bytes;
static uint32_t g_flash_bytes_used_cached;
static const uint8_t *g_active_blob;
static uint32_t g_active_entries_start;
static uint32_t g_active_entries_len;
static uint8_t s_blob_scratch[BOOT_SETTINGS_SCRATCH_MAX];

static inline uint32_t read_le_u32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static inline void write_le_u32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8U) & 0xFFU);
  p[2] = (uint8_t)((v >> 16U) & 0xFFU);
  p[3] = (uint8_t)((v >> 24U) & 0xFFU);
}

static inline uint32_t align_up_u32(uint32_t v, uint32_t a)
{
  return (v + (a - 1U)) & ~(a - 1U);
}

static uint32_t settings_crc_bytes(const uint8_t *bytes, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFFUL;
  for (uint32_t i = 0U; i < len; i++)
  {
    crc ^= bytes[i];
    for (uint32_t bit = 0U; bit < 8U; bit++)
    {
      crc = (crc >> 1U) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
    }
  }
  return ~crc;
}

static uint32_t ptr_table_phys_addr(uint32_t slot_index)
{
  return BOOT_METADATA_BASE_ADDR + BOOT_SETTINGS_BITMAP_BYTES + (slot_index * sizeof(uint32_t));
}

static uint32_t bitmap_count_used_slots(void)
{
  for (uint32_t i = 0U; i < BOOT_SETTINGS_INDEX_ENTRIES; i++)
  {
    uint32_t byte_idx = i / 8U;
    uint32_t bit_in_byte = i % 8U;
    uint8_t byte = *(const volatile uint8_t *)(BOOT_METADATA_BASE_ADDR + byte_idx);
    if (((uint32_t)(byte >> bit_in_byte) & 1U) != 0U)
    {
      return i;
    }
  }
  return BOOT_SETTINGS_INDEX_ENTRIES;
}

static uint32_t read_pointer_raw(uint32_t slot_index)
{
  return *(const volatile uint32_t *)ptr_table_phys_addr(slot_index);
}

static bool pointer_is_valid_reloc(uint32_t ptr)
{
  if ((ptr == 0U) || (ptr == 0xFFFFFFFFUL) || ((ptr % 4U) != 0U) ||
      (ptr < BOOT_SETTINGS_INDEX_TOTAL_BYTES))
  {
    return false;
  }

  return (BOOT_METADATA_BASE_ADDR + ptr + BOOT_KV_RECORD_HEADER_BYTES + BOOT_KV_RECORD_CRC_BYTES) <=
         BOOT_METADATA_LIMIT_ADDR;
}

static int flash_program_word(uint32_t addr, uint32_t value)
{
  return (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, value) == HAL_OK) ? 0 : -1;
}

static int flash_program_bytes(uint32_t addr, const uint8_t *data, uint32_t len)
{
  const uint32_t padded = align_up_u32(len, sizeof(uint32_t));
  uint8_t word_buf[sizeof(uint32_t)];

  for (uint32_t i = 0U; i < padded; i += sizeof(uint32_t))
  {
    for (uint32_t b = 0U; b < sizeof(uint32_t); b++)
    {
      const uint32_t ib = i + b;
      word_buf[b] = (ib < len) ? data[ib] : 0xFFU;
    }

    if (flash_program_word(addr + i, read_le_u32(word_buf)) != 0)
    {
      return -1;
    }
  }

  return 0;
}

static int program_bitmap_commit_bit(uint32_t bit_index)
{
  const uint32_t byte_pos = bit_index / 8U;
  const uint32_t bit_in_byte = bit_index % 8U;
  const uint32_t word_addr = BOOT_METADATA_BASE_ADDR + (byte_pos & ~3U);
  const uint32_t shift = ((byte_pos % 4U) * 8U) + bit_in_byte;
  const uint32_t old = *(const volatile uint32_t *)word_addr;
  const uint32_t mask = 1UL << shift;

  if ((old & mask) == 0U)
  {
    return -1;
  }

  return flash_program_word(word_addr, old & ~mask);
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

static uint32_t flash_blob_footprint(uint32_t record_size)
{
  return align_up_u32(record_size, BOOT_SETTINGS_ALIGNMENT);
}

static uint32_t recompute_flash_high_water_relative(void)
{
  uint32_t wm = BOOT_SETTINGS_INDEX_TOTAL_BYTES;
  const uint32_t used_slots = bitmap_count_used_slots();

  for (uint32_t i = 0U; i < used_slots; i++)
  {
    const uint32_t ptr = read_pointer_raw(i);
    if (!pointer_is_valid_reloc(ptr))
    {
      continue;
    }

    const uint8_t *base = (const uint8_t *)(uintptr_t)(BOOT_METADATA_BASE_ADDR + ptr);
    const uint32_t record_size = read_le_u32(base);
    if ((record_size < (BOOT_KV_RECORD_HEADER_BYTES + BOOT_KV_RECORD_CRC_BYTES)) ||
        (BOOT_METADATA_BASE_ADDR + ptr + record_size > BOOT_METADATA_LIMIT_ADDR))
    {
      continue;
    }

    const uint32_t end = ptr + flash_blob_footprint(record_size);
    if (end > wm)
    {
      wm = end;
    }
  }

  return wm;
}

static void ResetStoreState(void)
{
  memset(g_entries, 0, sizeof(g_entries));
  g_entry_count = 0U;
  g_store_dirty = false;
  g_store_loaded = false;
  g_entries_materialized = false;
  g_active_slot = 0U;
  g_active_settings_phys_addr = 0U;
  g_active_settings_blob_size_bytes = 0U;
  g_flash_bytes_used_cached = BOOT_SETTINGS_INDEX_TOTAL_BYTES;
  g_active_blob = 0;
  g_active_entries_start = 0U;
  g_active_entries_len = 0U;
}

static int FindRamEntryIndex(uint32_t key)
{
  for (uint16_t i = 0U; i < g_entry_count; i++)
  {
    if (g_entries[i].present && (g_entries[i].key == key))
    {
      return (int)i;
    }
  }
  return -1;
}

static bool scan_flash_entry_at(uint32_t pos, uint32_t *key_out, const uint8_t **value_out,
                                uint16_t *value_len_out, uint32_t *next_pos_out)
{
  if ((g_active_blob == 0) || (pos < g_active_entries_start) ||
      ((pos + BOOT_KV_ENTRY_HEADER_BYTES) > (g_active_entries_start + g_active_entries_len)))
  {
    return false;
  }

  const uint32_t key = read_le_u32(g_active_blob + pos);
  const uint8_t value_len = g_active_blob[pos + sizeof(uint32_t)];
  const uint32_t value_pos = pos + BOOT_KV_ENTRY_HEADER_BYTES;
  const uint32_t next_pos = value_pos + value_len;

  if ((value_pos + value_len) > (g_active_entries_start + g_active_entries_len) ||
      (next_pos > (g_active_entries_start + g_active_entries_len)))
  {
    return false;
  }

  if (key_out != 0)
  {
    *key_out = key;
  }
  if (value_out != 0)
  {
    *value_out = g_active_blob + value_pos;
  }
  if (value_len_out != 0)
  {
    *value_len_out = value_len;
  }
  if (next_pos_out != 0)
  {
    *next_pos_out = next_pos;
  }
  return true;
}

static int MaterializeEntriesFromActiveBlob(void)
{
  if (g_entries_materialized)
  {
    return 0;
  }

  if (g_active_blob == 0)
  {
    memset(g_entries, 0, sizeof(g_entries));
    g_entry_count = 0U;
    g_entries_materialized = true;
    return 0;
  }

  memset(g_entries, 0, sizeof(g_entries));
  g_entry_count = 0U;

  uint32_t pos = g_active_entries_start;
  while (pos < (g_active_entries_start + g_active_entries_len))
  {
    uint32_t key;
    const uint8_t *value;
    uint16_t value_len;
    uint32_t next_pos;

    if (!scan_flash_entry_at(pos, &key, &value, &value_len, &next_pos))
    {
      return -1;
    }

    if (value_len > BOOT_METADATA_KV_VALUE_MAX)
    {
      return -1;
    }

    if (g_entry_count >= BOOT_METADATA_STORED_KV_MAX)
    {
      return -1;
    }

    g_entries[g_entry_count].key = key;
    g_entries[g_entry_count].value_len = value_len;
    g_entries[g_entry_count].present = true;
    if (value_len != 0U)
    {
      memcpy(g_entries[g_entry_count].value, value, value_len);
    }
    g_entry_count++;
    pos = next_pos;
  }

  g_entries_materialized = true;
  return 0;
}

static bool ValidateSettingsObjectHeader(const uint8_t *blob, uint32_t blob_avail_max,
                                         uint32_t *record_total_out, uint32_t *entries_len_out)
{
  if (blob_avail_max < (BOOT_KV_RECORD_HEADER_BYTES + BOOT_KV_RECORD_CRC_BYTES))
  {
    return false;
  }

  const uint32_t record_total = read_le_u32(blob);
  const uint32_t magic = read_le_u32(blob + 4U);
  const uint32_t version = read_le_u32(blob + 8U);
  const uint32_t entries_len = read_le_u32(blob + 12U);

  if ((record_total < (BOOT_KV_RECORD_HEADER_BYTES + BOOT_KV_RECORD_CRC_BYTES)) ||
      (record_total > blob_avail_max) || (record_total > BOOT_SETTINGS_SCRATCH_MAX) ||
      (magic != BOOT_METADATA_MAGIC) || (version != BOOT_METADATA_VERSION) ||
      ((BOOT_KV_RECORD_HEADER_BYTES + entries_len + BOOT_KV_RECORD_CRC_BYTES) > record_total))
  {
    return false;
  }

  const uint32_t crc_calc = settings_crc_bytes(blob, record_total - BOOT_KV_RECORD_CRC_BYTES);
  const uint32_t crc_flash = read_le_u32(blob + (record_total - BOOT_KV_RECORD_CRC_BYTES));
  if (crc_calc != crc_flash)
  {
    return false;
  }

  *record_total_out = record_total;
  *entries_len_out = entries_len;
  return true;
}

static bool ValidateEntryWalk(const uint8_t *entries, uint32_t entries_len)
{
  uint32_t pos = 0U;
  uint16_t count = 0U;

  while (pos < entries_len)
  {
    if ((pos + BOOT_KV_ENTRY_HEADER_BYTES) > entries_len)
    {
      return false;
    }
    const uint8_t value_len = entries[pos + sizeof(uint32_t)];
    pos += BOOT_KV_ENTRY_HEADER_BYTES;
    if ((pos + value_len) > entries_len)
    {
      return false;
    }
    pos += value_len;
    count++;
    if (count > BOOT_METADATA_STORED_KV_MAX)
    {
      return false;
    }
  }

  return true;
}

static bool ValidateCurrentStore(void)
{
  if ((g_active_blob == 0) || (g_active_settings_blob_size_bytes == 0U))
  {
    return true;
  }

  uint32_t record_total;
  uint32_t entries_len;
  if (!ValidateSettingsObjectHeader(g_active_blob, g_active_settings_blob_size_bytes,
                                    &record_total, &entries_len))
  {
    return false;
  }
  (void)record_total;
  return ValidateEntryWalk(g_active_blob + BOOT_KV_RECORD_HEADER_BYTES, entries_len);
}

static bool ScanFlashForActiveSettingsObject(void)
{
  const uint32_t used = bitmap_count_used_slots();
  if (used == 0U)
  {
    return false;
  }

  const uint32_t newest_idx = used - 1U;
  const uint32_t ptr = read_pointer_raw(newest_idx);
  if (!pointer_is_valid_reloc(ptr))
  {
    return false;
  }

  const uint8_t *blob_start = (const uint8_t *)(uintptr_t)(BOOT_METADATA_BASE_ADDR + ptr);
  const uint32_t bound = BOOT_METADATA_LIMIT_ADDR - (BOOT_METADATA_BASE_ADDR + ptr);
  uint32_t record_total;
  uint32_t entries_len;
  if (!ValidateSettingsObjectHeader(blob_start, bound, &record_total, &entries_len) ||
      !ValidateEntryWalk(blob_start + BOOT_KV_RECORD_HEADER_BYTES, entries_len))
  {
    return false;
  }

  g_active_slot = newest_idx;
  g_active_settings_phys_addr = (uint32_t)(uintptr_t)blob_start;
  g_active_settings_blob_size_bytes = record_total;
  g_flash_bytes_used_cached = recompute_flash_high_water_relative();
  g_active_blob = blob_start;
  g_active_entries_start = BOOT_KV_RECORD_HEADER_BYTES;
  g_active_entries_len = entries_len;
  return true;
}

static void SortRamEntriesByKey(void)
{
  for (uint16_t i = 0U; (i + 1U) < g_entry_count; i++)
  {
    for (uint16_t j = (uint16_t)(i + 1U); j < g_entry_count; j++)
    {
      if (g_entries[j].key < g_entries[i].key)
      {
        BootMetadataRamEntry tmp = g_entries[i];
        g_entries[i] = g_entries[j];
        g_entries[j] = tmp;
      }
    }
  }
}

static int SerializeSettingsObject(uint32_t *blob_len_out)
{
  uint32_t entries_ofs = BOOT_KV_RECORD_HEADER_BYTES;

  memset(s_blob_scratch, 0xFF, sizeof(s_blob_scratch));
  SortRamEntriesByKey();

  for (uint16_t i = 0U; i < g_entry_count; i++)
  {
    if (!g_entries[i].present)
    {
      continue;
    }
    if (g_entries[i].value_len > UINT8_MAX)
    {
      return -1;
    }
    const uint32_t need = entries_ofs + BOOT_KV_ENTRY_HEADER_BYTES + g_entries[i].value_len;
    if ((need + BOOT_KV_RECORD_CRC_BYTES) > BOOT_SETTINGS_SCRATCH_MAX)
    {
      return -1;
    }

    write_le_u32(s_blob_scratch + entries_ofs, g_entries[i].key);
    entries_ofs += sizeof(uint32_t);
    s_blob_scratch[entries_ofs++] = (uint8_t)g_entries[i].value_len;
    if (g_entries[i].value_len != 0U)
    {
      memcpy(s_blob_scratch + entries_ofs, g_entries[i].value, g_entries[i].value_len);
      entries_ofs += g_entries[i].value_len;
    }
  }

  const uint32_t crc_ofs = align_up_u32(entries_ofs, BOOT_SETTINGS_ALIGNMENT);
  if ((crc_ofs + BOOT_KV_RECORD_CRC_BYTES) > BOOT_SETTINGS_SCRATCH_MAX)
  {
    return -1;
  }

  write_le_u32(s_blob_scratch, crc_ofs + BOOT_KV_RECORD_CRC_BYTES);
  write_le_u32(s_blob_scratch + 4U, BOOT_METADATA_MAGIC);
  write_le_u32(s_blob_scratch + 8U, BOOT_METADATA_VERSION);
  write_le_u32(s_blob_scratch + 12U, entries_ofs - BOOT_KV_RECORD_HEADER_BYTES);
  write_le_u32(s_blob_scratch + crc_ofs, settings_crc_bytes(s_blob_scratch, crc_ofs));
  *blob_len_out = crc_ofs + BOOT_KV_RECORD_CRC_BYTES;
  return 0;
}

static int blob_append_physical(uint32_t blob_len)
{
  const uint32_t padded_len = flash_blob_footprint(blob_len);
  uint32_t slots_used_now;
  uint32_t heap_water;
  uint32_t slot_ix;

restart_after_erase_label:
  slots_used_now = bitmap_count_used_slots();
  heap_water = (slots_used_now == 0U) ? BOOT_SETTINGS_INDEX_TOTAL_BYTES : recompute_flash_high_water_relative();

  if (slots_used_now >= BOOT_SETTINGS_INDEX_ENTRIES)
  {
    if (metadata_erase_sector() != 0)
    {
      return -1;
    }
    slots_used_now = 0U;
    heap_water = BOOT_SETTINGS_INDEX_TOTAL_BYTES;
  }

  if ((heap_water + padded_len > BOOT_METADATA_SIZE_BYTES) || (heap_water + padded_len < heap_water))
  {
    if (metadata_erase_sector() != 0)
    {
      return -1;
    }
    goto restart_after_erase_label;
  }

  slot_ix = slots_used_now;
  const uint32_t phy_dst_rel = heap_water;
  const uint32_t phy_dst_phys = BOOT_METADATA_BASE_ADDR + phy_dst_rel;
  if ((flash_program_bytes(phy_dst_phys, s_blob_scratch, padded_len) != 0) ||
      (flash_program_word(ptr_table_phys_addr(slot_ix), phy_dst_rel) != 0) ||
      (program_bitmap_commit_bit(slot_ix) != 0))
  {
    return -1;
  }

  g_active_slot = slot_ix;
  g_active_settings_phys_addr = phy_dst_phys;
  g_active_settings_blob_size_bytes = blob_len;
  g_flash_bytes_used_cached = recompute_flash_high_water_relative();
  g_active_blob = (const uint8_t *)(uintptr_t)phy_dst_phys;
  g_active_entries_start = BOOT_KV_RECORD_HEADER_BYTES;
  g_active_entries_len = read_le_u32(g_active_blob + 12U);
  return 0;
}

static uint32_t metadata_app_storage_hash(const char *key)
{
  uint32_t h = 2166136261UL;

  if (key == 0)
  {
    return 0U;
  }

  for (;;)
  {
    const uint8_t b = (uint8_t)*key++;
    if (b == (uint8_t)'\0')
    {
      break;
    }
    h ^= b;
    h *= 16777619UL;
  }

  return (BOOT_KV_APP_STORAGE_BASE | (h & BOOT_KV_APP_STORAGE_MASK));
}

int boot_metadata_initialize(void)
{
  ResetStoreState();
  g_flash_bytes_used_cached = recompute_flash_high_water_relative();

  if (!ScanFlashForActiveSettingsObject())
  {
    if (boot_metadata_clear_sector_and_reset_store() != 0)
    {
      return -1;
    }
    g_store_loaded = true;
    return 1;
  }

  g_store_loaded = true;
  return 0;
}

int boot_metadata_clear_sector_and_reset_store(void)
{
  if (HAL_FLASH_Unlock() != HAL_OK)
  {
    return -1;
  }
  const int erase_result = metadata_erase_sector();
  (void)HAL_FLASH_Lock();
  ResetStoreState();
  g_store_loaded = true;
  return erase_result;
}

int boot_metadata_get(uint32_t key, BootMetadataValueView *value_out)
{
  if (value_out != 0)
  {
    value_out->value = 0;
    value_out->value_len = 0U;
  }
  if (value_out == 0)
  {
    return -1;
  }

  if (g_entries_materialized)
  {
    const int ram_index = FindRamEntryIndex(key);
    if (ram_index >= 0)
    {
      value_out->value = g_entries[ram_index].value;
      value_out->value_len = g_entries[ram_index].value_len;
      return 0;
    }
  }

  if (g_active_blob == 0)
  {
    return -1;
  }

  uint32_t pos = g_active_entries_start;
  while (pos < (g_active_entries_start + g_active_entries_len))
  {
    uint32_t found_key;
    const uint8_t *value;
    uint16_t value_len;
    uint32_t next_pos;

    if (!scan_flash_entry_at(pos, &found_key, &value, &value_len, &next_pos))
    {
      return -1;
    }
    if (found_key == key)
    {
      value_out->value = value;
      value_out->value_len = value_len;
      return 0;
    }
    pos = next_pos;
  }

  return -1;
}

int boot_metadata_set(uint32_t key, const uint8_t *value, uint16_t value_len)
{
  if (((value_len != 0U) && (value == 0)) || (value_len > BOOT_METADATA_KV_VALUE_MAX) ||
      (value_len > UINT8_MAX))
  {
    return -1;
  }

  if (!g_entries_materialized && (MaterializeEntriesFromActiveBlob() != 0))
  {
    return -1;
  }
  int index = FindRamEntryIndex(key);
  if (index < 0)
  {
    if (g_entry_count >= BOOT_METADATA_STORED_KV_MAX)
    {
      return -1;
    }
    index = (int)g_entry_count++;
    g_entries[index].key = key;
    g_entries[index].present = true;
  }

  if ((g_entries[index].value_len == value_len) &&
      ((value_len == 0U) || (memcmp(g_entries[index].value, value, value_len) == 0)))
  {
    return 0;
  }

  g_entries[index].value_len = value_len;
  if (value_len != 0U)
  {
    memcpy(g_entries[index].value, value, value_len);
  }
  g_store_dirty = true;
  return 0;
}

int boot_metadata_set_many(const BootMetadataEntryView *entries, uint16_t entry_count)
{
  if ((entries == 0) || (entry_count == 0U))
  {
    return -1;
  }

  for (uint16_t i = 0U; i < entry_count; i++)
  {
    if (boot_metadata_set(entries[i].key, entries[i].value.value, entries[i].value.value_len) != 0)
    {
      return -1;
    }
  }

  return 0;
}

int boot_metadata_remove(uint32_t key)
{
  if (!g_entries_materialized && (MaterializeEntriesFromActiveBlob() != 0))
  {
    return -1;
  }
  const int index = FindRamEntryIndex(key);
  if (index < 0)
  {
    return 0;
  }

  for (uint16_t i = (uint16_t)index; (i + 1U) < g_entry_count; i++)
  {
    g_entries[i] = g_entries[i + 1U];
  }
  g_entry_count--;
  memset(&g_entries[g_entry_count], 0, sizeof(g_entries[g_entry_count]));
  g_store_dirty = true;
  return 0;
}

int boot_metadata_save_to_flash(void)
{
  uint32_t blob_len;

  if (!g_store_dirty && (g_active_settings_blob_size_bytes != 0U))
  {
    return 0;
  }
  if (SerializeSettingsObject(&blob_len) != 0)
  {
    return -1;
  }
  if (HAL_FLASH_Unlock() != HAL_OK)
  {
    return -1;
  }
  const int result = blob_append_physical(blob_len);
  (void)HAL_FLASH_Lock();
  if (result != 0)
  {
    return -1;
  }
  g_store_dirty = false;
  g_entries_materialized = false;
  (void)MaterializeEntriesFromActiveBlob();
  return 0;
}

uint16_t boot_metadata_count(void)
{
  if (!g_entries_materialized)
  {
    (void)MaterializeEntriesFromActiveBlob();
  }
  return g_entry_count;
}

int boot_metadata_get_by_index(uint16_t index, BootMetadataEntryView *entry_out)
{
  if (!g_entries_materialized && (MaterializeEntriesFromActiveBlob() != 0))
  {
    return -1;
  }
  if ((entry_out == 0) || (index >= g_entry_count))
  {
    return -1;
  }

  entry_out->key = g_entries[index].key;
  entry_out->value.value = g_entries[index].value;
  entry_out->value.value_len = g_entries[index].value_len;
  return 0;
}

bool boot_metadata_validate_current_store(void)
{
  return g_store_loaded && ValidateCurrentStore();
}

uint32_t boot_metadata_get_current_settings_slot(void)
{
  return g_active_slot;
}

uint32_t boot_metadata_get_flash_bytes_used(void)
{
  return g_flash_bytes_used_cached;
}

uint32_t boot_metadata_get_flash_bytes_remaining(void)
{
  return (g_flash_bytes_used_cached >= BOOT_METADATA_SIZE_BYTES) ? 0U :
         (BOOT_METADATA_SIZE_BYTES - g_flash_bytes_used_cached);
}

uint32_t boot_metadata_get_current_settings_object_phys_addr(void)
{
  return g_active_settings_phys_addr;
}

uint32_t boot_metadata_get_current_settings_object_size_bytes(void)
{
  return g_active_settings_blob_size_bytes;
}

uint16_t boot_metadata_stored_kv_count(void)
{
  return boot_metadata_count();
}

int boot_metadata_read_stored_kv_raw(uint16_t index_one_based, uint32_t *key_out,
                                     uint8_t *value_out, uint16_t value_cap, uint16_t *value_len_out)
{
  BootMetadataEntryView entry;
  if (value_len_out != 0)
  {
    *value_len_out = 0U;
  }
  if ((index_one_based == 0U) || (key_out == 0) || (value_out == 0) || (value_len_out == 0) ||
      (boot_metadata_get_by_index((uint16_t)(index_one_based - 1U), &entry) != 0))
  {
    return -1;
  }
  if (value_cap < entry.value.value_len)
  {
    *value_len_out = entry.value.value_len;
    return -1;
  }
  *key_out = entry.key;
  *value_len_out = entry.value.value_len;
  if (entry.value.value_len != 0U)
  {
    memcpy(value_out, entry.value.value, entry.value.value_len);
  }
  return 0;
}

int boot_metadata_storage_read_string_key(const char *key, void *data, size_t max_length, size_t *length_out)
{
  BootMetadataValueView value;
  if (length_out != 0)
  {
    *length_out = 0U;
  }
  if ((key == 0) || (data == 0) || (length_out == 0))
  {
    return -1;
  }
  if (boot_metadata_get(metadata_app_storage_hash(key), &value) != 0)
  {
    return 0;
  }
  if (max_length < (size_t)value.value_len)
  {
    *length_out = value.value_len;
    return -1;
  }
  if (value.value_len != 0U)
  {
    memcpy(data, value.value, value.value_len);
  }
  *length_out = value.value_len;
  return 0;
}

int boot_metadata_storage_write_string_key(const char *key, const void *data, size_t length)
{
  if ((key == 0) || ((length != 0U) && (data == 0)) || (length > BOOT_METADATA_KV_VALUE_MAX))
  {
    return -1;
  }
  return boot_metadata_set(metadata_app_storage_hash(key), (const uint8_t *)data, (uint16_t)length);
}
