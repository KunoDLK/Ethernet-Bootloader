#include "boot_metadata.h"

#include "boot_kv_nodes.h"
#include "lwipopts.h"
#include "stm32f4xx_hal.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/**
 * Private RAM image for encode/decode (mirrors canonical BOOT_KV_* fields + unused padding).
 * External code must use boot_metadata_kv_* or the typed getters/setters—not this layout.
 */
typedef struct
{
  uint32_t magic;
  uint32_t version;
  uint32_t sequence;
  uint32_t app_valid;
  uint32_t app_disabled;
  uint32_t app_version;
  uint32_t last_fault_reason;
  uint32_t last_fault_pc;
  uint32_t last_fault_lr;
  uint8_t net_ipv4_addr[4];
  uint8_t net_ipv4_subnet[4];
  uint8_t net_ipv4_gateway[4];
  uint8_t net_mac[6];
  uint8_t reserved[2];
  uint32_t hardware_poll_period_ms;
  uint8_t rail_a_mode;
  uint8_t rail_b_mode;
  uint8_t net_dhcp_enabled;
  uint8_t reserved_pad;
  uint32_t crc32;
} BootKvRam;

_Static_assert(sizeof(BootKvRam) <= 128U, "KV RAM decode image unexpectedly large");

#define BOOT_KV_EXTRA_MAX          (48U)
#define BOOT_KV_EXTRA_VALUE_MAX    (236U)
#define BOOT_SETTINGS_SCRATCH_MAX  (4096U)
#define BOOT_SETTINGS_ALIGNMENT    (8U)

typedef struct
{
  uint32_t node_id;
  uint16_t length;
  uint8_t value[BOOT_KV_EXTRA_VALUE_MAX];
} BootExtraGeneric;

typedef struct
{
  uint32_t node_id;
  uint32_t value;
} BootU32Decoded;

static BootKvRam g_kv_ram;
static uint32_t g_active_slot;
static uint32_t g_active_settings_phys_addr;
static uint32_t g_active_settings_blob_size_bytes;
static uint32_t g_flash_bytes_used_cached;
static BootExtraGeneric g_extra_kv[BOOT_KV_EXTRA_MAX];
static uint8_t g_extra_count;
static BootU32Decoded g_u32_scratch[24];
static uint8_t g_u32_decoded_count;

#define BOOT_METADATA_SORTED_CAP (BOOT_KV_EXTRA_MAX + 48U)
static uint32_t g_sorted_ids[BOOT_METADATA_SORTED_CAP];
static uint16_t g_sorted_count;

static uint8_t s_blob_scratch[BOOT_SETTINGS_SCRATCH_MAX];

/** One canonical node: optional u32 pair lane and/or TLV blob in the KV section. */
typedef struct
{
  uint32_t node_id;
  uint8_t in_u32_lane;
  uint8_t in_tlv;
  uint16_t field_offset;
  uint8_t tlv_len;
} BootCanonEntry;

static const BootCanonEntry k_boot_canon[] = {
    { BOOT_KV_SEQUENCE,      1U, 0U, (uint16_t)offsetof(BootKvRam, sequence), 0U},
    { BOOT_KV_APP_VALID,     1U, 0U, (uint16_t)offsetof(BootKvRam, app_valid), 0U},
    { BOOT_KV_APP_DISABLED,  1U, 0U, (uint16_t)offsetof(BootKvRam, app_disabled), 0U},
    { BOOT_KV_APP_VERSION,   1U, 0U, (uint16_t)offsetof(BootKvRam, app_version), 0U},
    { BOOT_KV_FAULT_REASON,  1U, 0U, (uint16_t)offsetof(BootKvRam, last_fault_reason), 0U},
    { BOOT_KV_FAULT_PC,      1U, 0U, (uint16_t)offsetof(BootKvRam, last_fault_pc), 0U},
    { BOOT_KV_FAULT_LR,      1U, 0U, (uint16_t)offsetof(BootKvRam, last_fault_lr), 0U},
    { BOOT_KV_IPV4_ADDR,     0U, 1U, (uint16_t)offsetof(BootKvRam, net_ipv4_addr), 4U},
    { BOOT_KV_IPV4_SUBNET,   0U, 1U, (uint16_t)offsetof(BootKvRam, net_ipv4_subnet), 4U},
    { BOOT_KV_IPV4_GW,       0U, 1U, (uint16_t)offsetof(BootKvRam, net_ipv4_gateway), 4U},
    { BOOT_KV_NET_MAC,       0U, 1U, (uint16_t)offsetof(BootKvRam, net_mac), 6U},
    { BOOT_KV_HW_POLL_MS,    1U, 0U, (uint16_t)offsetof(BootKvRam, hardware_poll_period_ms), 0U},
    { BOOT_KV_RAIL_A,        1U, 1U, (uint16_t)offsetof(BootKvRam, rail_a_mode), 1U},
    { BOOT_KV_RAIL_B,        1U, 1U, (uint16_t)offsetof(BootKvRam, rail_b_mode), 1U},
    { BOOT_KV_NET_DHCP,      0U, 1U, (uint16_t)offsetof(BootKvRam, net_dhcp_enabled), 1U},
};
#define BOOT_CANON_COUNT ((uint32_t)(sizeof(k_boot_canon) / sizeof(k_boot_canon[0])))

static inline uint32_t read_le_u32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static inline uint16_t read_le_u16(const uint8_t *p)
{
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8U);
}

static inline void write_le_u16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8U) & 0xFFU);
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
  uint32_t limit = BOOT_SETTINGS_INDEX_ENTRIES;
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

static uint32_t read_pointer_raw(uint32_t slot_index)
{
  return *(const volatile uint32_t *)ptr_table_phys_addr(slot_index);
}

static bool pointer_is_valid_reloc(uint32_t ptr)
{
  if ((ptr == 0U) || (ptr == 0xFFFFFFFFUL))
  {
    return false;
  }
  if ((ptr % 4U) != 0U)
  {
    return false;
  }
  if (ptr < BOOT_SETTINGS_INDEX_TOTAL_BYTES)
  {
    return false;
  }
  return (BOOT_METADATA_BASE_ADDR + ptr + 24U) <= BOOT_METADATA_LIMIT_ADDR;
}

static int flash_program_word(uint32_t addr, uint32_t value)
{
  return (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, value) == HAL_OK) ? 0 : -1;
}

static int flash_program_bytes(uint32_t addr, const uint8_t *data, uint32_t len)
{
  uint32_t padded = align_up_u32(len, sizeof(uint32_t));
  uint8_t word_buf[sizeof(uint32_t)];

  for (uint32_t i = 0U; i < padded; i += sizeof(uint32_t))
  {
    for (uint32_t b = 0U; b < sizeof(uint32_t); b++)
    {
      uint32_t ib = i + b;
      word_buf[b] = (ib < len) ? data[ib] : 0xFFU;
    }
    uint32_t w = read_le_u32(word_buf);
    if (flash_program_word(addr + i, w) != 0)
    {
      return -1;
    }
  }

  return 0;
}

static int program_bitmap_commit_bit(uint32_t bit_index)
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
  uint32_t used_slots = bitmap_count_used_slots();

  for (uint32_t i = 0U; i < used_slots; i++)
  {
    uint32_t ptr = read_pointer_raw(i);
    uint32_t record_size;
    const uint8_t *base;
    uint32_t end;

    if (!pointer_is_valid_reloc(ptr))
    {
      continue;
    }

    base = (const uint8_t *)(uintptr_t)(BOOT_METADATA_BASE_ADDR + ptr);
    record_size = read_le_u32(base);
    if (record_size < 24U)
    {
      continue;
    }

    end = ptr + flash_blob_footprint(record_size);
    if (end > wm)
    {
      wm = end;
    }
  }

  return wm;
}

static void extras_clear_all(void)
{
  memset(g_extra_kv, 0, sizeof(g_extra_kv));
  g_extra_count = 0U;
  g_u32_decoded_count = 0U;
}

static uint32_t metadata_app_storage_hash(const char *key)
{
  uint32_t h = 2166136261UL;

  if (key == NULL)
  {
    return 0U;
  }

  for (;;)
  {
    uint8_t b = (uint8_t)*key++;
    if (b == (uint8_t)'\0')
    {
      break;
    }

    h ^= b;
    h *= 16777619UL;
  }

  return (BOOT_KV_APP_STORAGE_BASE | (h & BOOT_KV_APP_STORAGE_MASK));
}

static bool extra_contains_node(uint32_t node_id, uint16_t *out_index)
{
  for (uint8_t i = 0U; i < g_extra_count; i++)
  {
    if (g_extra_kv[i].node_id == node_id)
    {
      if (out_index != NULL)
      {
        *out_index = i;
      }
      return true;
    }
  }
  return false;
}

static int extra_try_put(uint32_t node_id, const uint8_t *value, uint16_t length)
{
  uint16_t existing;

  if (length > BOOT_KV_EXTRA_VALUE_MAX)
  {
    return -1;
  }

  if (extra_contains_node(node_id, &existing))
  {
    memcpy(g_extra_kv[existing].value, value, length);
    g_extra_kv[existing].length = length;
    return 0;
  }

  if (g_extra_count >= BOOT_KV_EXTRA_MAX)
  {
    return -1;
  }

  g_extra_kv[g_extra_count].node_id = node_id;
  g_extra_kv[g_extra_count].length = length;
  if (length > 0U)
  {
    memcpy(g_extra_kv[g_extra_count].value, value, length);
  }
  g_extra_count++;
  return 0;
}

static uint32_t u32_decoded_fetch(uint32_t node_id)
{
  for (uint8_t i = 0U; i < g_u32_decoded_count; i++)
  {
    if (g_u32_scratch[i].node_id == node_id)
    {
      return g_u32_scratch[i].value;
    }
  }

  return 0U;
}

static uint32_t canon_u32_lane_read(const BootKvRam *m, const BootCanonEntry *e)
{
  const uint8_t *base = (const uint8_t *)m;

  if (e->tlv_len == 1U)
  {
    return (uint32_t)*(const uint8_t *)(base + e->field_offset);
  }

  return *(const uint32_t *)(uintptr_t)(base + e->field_offset);
}

static void canon_u32_lane_write(BootKvRam *m, const BootCanonEntry *e, uint32_t v)
{
  uint8_t *base = (uint8_t *)m;

  if (e->node_id == BOOT_KV_HW_POLL_MS)
  {
    if (v < BOOT_METADATA_DEFAULT_HARDWARE_POLL_PERIOD_MS)
    {
      v = BOOT_METADATA_DEFAULT_HARDWARE_POLL_PERIOD_MS;
    }
    *(uint32_t *)(uintptr_t)(base + e->field_offset) = v;
    return;
  }

  if (e->tlv_len == 1U)
  {
    if (v <= 2U)
    {
      *(uint8_t *)(base + e->field_offset) = (uint8_t)v;
    }

    return;
  }

  *(uint32_t *)(uintptr_t)(base + e->field_offset) = v;
}

static const BootCanonEntry *canon_find_tlv_row(uint32_t node_id)
{
  for (uint32_t i = 0U; i < BOOT_CANON_COUNT; i++)
  {
    if ((k_boot_canon[i].node_id == node_id) && (k_boot_canon[i].in_tlv != 0U))
    {
      return &k_boot_canon[i];
    }
  }

  return NULL;
}

static const BootCanonEntry *canon_find_u32_lane_row(uint32_t node_id)
{
  for (uint32_t i = 0U; i < BOOT_CANON_COUNT; i++)
  {
    if ((k_boot_canon[i].node_id == node_id) && (k_boot_canon[i].in_u32_lane != 0U))
    {
      return &k_boot_canon[i];
    }
  }

  return NULL;
}

static bool extra_read_payload(uint32_t node_id, const uint8_t **payload_out, uint16_t *len_out)
{
  uint16_t idx;

  if (extra_contains_node(node_id, &idx))
  {
    if (payload_out != NULL)
    {
      *payload_out = g_extra_kv[idx].value;
    }

    if (len_out != NULL)
    {
      *len_out = g_extra_kv[idx].length;
    }

    return true;
  }

  return false;
}

static void apply_u32_decoded_overlay_from_scratch(BootKvRam *meta)
{
  for (uint32_t i = 0U; i < BOOT_CANON_COUNT; i++)
  {
    const BootCanonEntry *e = &k_boot_canon[i];
    uint32_t v;

    if (e->in_u32_lane == 0U)
    {
      continue;
    }

    v = u32_decoded_fetch(e->node_id);
    canon_u32_lane_write(meta, e, v);
  }
}

static void metadata_defaults_ram(void)
{
  g_kv_ram.magic = BOOT_METADATA_MAGIC;
  g_kv_ram.version = BOOT_METADATA_VERSION;
  g_kv_ram.sequence = 0U;
  g_kv_ram.app_valid = 0U;
  g_kv_ram.app_disabled = 0U;
  g_kv_ram.app_version = 0U;
  g_kv_ram.last_fault_reason = BOOT_APP_FAULT_NONE;
  g_kv_ram.last_fault_pc = 0U;
  g_kv_ram.last_fault_lr = 0U;
  g_kv_ram.net_ipv4_addr[0] = RESIDENT_IPV4_ADDR0;
  g_kv_ram.net_ipv4_addr[1] = RESIDENT_IPV4_ADDR1;
  g_kv_ram.net_ipv4_addr[2] = RESIDENT_IPV4_ADDR2;
  g_kv_ram.net_ipv4_addr[3] = RESIDENT_IPV4_ADDR3;
  g_kv_ram.net_ipv4_subnet[0] = RESIDENT_IPV4_MASK0;
  g_kv_ram.net_ipv4_subnet[1] = RESIDENT_IPV4_MASK1;
  g_kv_ram.net_ipv4_subnet[2] = RESIDENT_IPV4_MASK2;
  g_kv_ram.net_ipv4_subnet[3] = RESIDENT_IPV4_MASK3;
  g_kv_ram.net_ipv4_gateway[0] = RESIDENT_IPV4_GW0;
  g_kv_ram.net_ipv4_gateway[1] = RESIDENT_IPV4_GW1;
  g_kv_ram.net_ipv4_gateway[2] = RESIDENT_IPV4_GW2;
  g_kv_ram.net_ipv4_gateway[3] = RESIDENT_IPV4_GW3;
  g_kv_ram.net_mac[0] = 0x30U;
  g_kv_ram.net_mac[1] = 0x3DU;
  g_kv_ram.net_mac[2] = 0x51U;
  g_kv_ram.net_mac[3] = 0xBAU;
  g_kv_ram.net_mac[4] = 0x00U;
  g_kv_ram.net_mac[5] = 0x00U;
  g_kv_ram.hardware_poll_period_ms = BOOT_METADATA_DEFAULT_HARDWARE_POLL_PERIOD_MS;
  g_kv_ram.rail_a_mode = BOOT_METADATA_DEFAULT_RAIL_MODE;
  g_kv_ram.rail_b_mode = BOOT_METADATA_DEFAULT_RAIL_MODE;
  g_kv_ram.net_dhcp_enabled = 1U;
  g_kv_ram.reserved_pad = 0U;
  g_kv_ram.crc32 = 0U;
}

static int apply_generic_tlv(uint32_t node_id, const uint8_t *val, uint16_t vlen)
{
  const BootCanonEntry *e = canon_find_tlv_row(node_id);
  uint8_t *slot;

  if (e != NULL)
  {
    if ((vlen != e->tlv_len) || (vlen == 0U))
    {
      return 0;
    }

    if ((node_id == BOOT_KV_NET_DHCP) && (val[0] > 1U))
    {
      return 0;
    }

    if ((e->tlv_len == 1U) && (val[0] > 2U))
    {
      return 0;
    }

    slot = ((uint8_t *)&g_kv_ram) + e->field_offset;
    memcpy(slot, val, vlen);
    return 0;
  }

  (void)extra_try_put(node_id, val, vlen);
  return 0;
}

static int parse_and_load_blob(const uint8_t *blob, uint32_t blob_avail_max)
{
  uint32_t record_total;
  uint32_t crc_calc;
  uint32_t crc_flash;
  uint32_t generic_len;
  uint32_t generic_start;
  uint32_t pos;
  uint32_t generic_end;
  uint32_t off;
  uint32_t u8_pair_count;
  uint32_t u32_pair_count;
  uint32_t avail;

  if (blob_avail_max < 16U)
  {
    return -1;
  }

  record_total = read_le_u32(blob);
  if ((record_total < 24U) || (record_total > blob_avail_max) || (record_total > BOOT_SETTINGS_SCRATCH_MAX))
  {
    return -1;
  }

  crc_calc = settings_crc_bytes(blob, record_total - sizeof(uint32_t));
  crc_flash = read_le_u32(blob + (record_total - sizeof(uint32_t)));
  if (crc_calc != crc_flash)
  {
    return -1;
  }

  extras_clear_all();
  metadata_defaults_ram();

  generic_len = read_le_u32(blob + sizeof(uint32_t));
  generic_start = sizeof(uint32_t) + sizeof(uint32_t);

  off = generic_start + generic_len;
  if ((off > record_total - sizeof(uint32_t)) || (generic_len > BOOT_SETTINGS_SCRATCH_MAX))
  {
    return -1;
  }

  generic_end = generic_start + generic_len;
  pos = generic_start;

  while (pos + sizeof(uint32_t) + sizeof(uint16_t) <= generic_end)
  {
    uint32_t nid;
    uint16_t vl;

    nid = read_le_u32(blob + pos);
    vl = read_le_u16(blob + pos + sizeof(uint32_t));
    pos += sizeof(uint32_t) + sizeof(uint16_t);

    if ((vl > BOOT_KV_EXTRA_VALUE_MAX) || (pos + vl > generic_end))
    {
      return -1;
    }

    (void)apply_generic_tlv(nid, blob + pos, vl);

    pos += vl;
    pos = align_up_u32(pos, 4U);
  }

  if (pos != generic_end)
  {
    return -1;
  }

  off = generic_end;
  off = align_up_u32(off, 4U);
  avail = record_total - sizeof(uint32_t);
  if (off + sizeof(uint32_t) > avail)
  {
    return -1;
  }

  u8_pair_count = read_le_u32(blob + off);
  off += sizeof(uint32_t);

  if (off + u8_pair_count * 2U > avail)
  {
    return -1;
  }

  for (uint32_t i = 0U; i < u8_pair_count; i++)
  {
    uint8_t k8 = blob[off++];
    uint8_t v8 = blob[off++];
    (void)apply_generic_tlv((uint32_t)k8, &v8, 1U);
  }

  off = align_up_u32(off, 4U);

  if (off + sizeof(uint32_t) > avail)
  {
    return -1;
  }

  u32_pair_count = read_le_u32(blob + off);
  off += sizeof(uint32_t);

  if (off + u32_pair_count * 8U > avail)
  {
    return -1;
  }

  g_u32_decoded_count = 0U;
  for (uint32_t i = 0U; i < u32_pair_count; i++)
  {
    uint32_t nid = read_le_u32(blob + off);
    uint32_t v = read_le_u32(blob + off + sizeof(uint32_t));
    off += 8U;

    if (g_u32_decoded_count < sizeof(g_u32_scratch) / sizeof(g_u32_scratch[0]))
    {
      g_u32_scratch[g_u32_decoded_count].node_id = nid;
      g_u32_scratch[g_u32_decoded_count].value = v;
      g_u32_decoded_count++;
    }
  }

  apply_u32_decoded_overlay_from_scratch(&g_kv_ram);

  g_kv_ram.magic = BOOT_METADATA_MAGIC;
  g_kv_ram.version = BOOT_METADATA_VERSION;
  return 0;
}

static int append_tlv_generic(uint8_t *blob, uint32_t *ofs_inout, uint32_t max_len, uint32_t node_id,
                              const uint8_t *data, uint16_t data_len)
{
  uint32_t ofs = *ofs_inout;
  uint32_t need =
      ofs + sizeof(uint32_t) + sizeof(uint16_t) + data_len + 4U /* pad upper bound */;

  if (need > max_len)
  {
    return -1;
  }

  write_le_u32(blob + ofs, node_id);
  ofs += sizeof(uint32_t);
  write_le_u16(blob + ofs, data_len);
  ofs += sizeof(uint16_t);

  if (data_len != 0U)
  {
    memcpy(blob + ofs, data, data_len);
    ofs += data_len;
  }

  ofs = align_up_u32(ofs, 4U);
  *ofs_inout = ofs;
  return 0;
}

static void sort_extra_ids_by_node(void)
{
  uint8_t i;
  uint8_t j;

  for (i = 0U; i + 1U < g_extra_count; i++)
  {
    for (j = (uint8_t)(i + 1U); j < g_extra_count; j++)
    {
      if (g_extra_kv[j].node_id < g_extra_kv[i].node_id)
      {
        BootExtraGeneric tmp = g_extra_kv[i];
        g_extra_kv[i] = g_extra_kv[j];
        g_extra_kv[j] = tmp;
      }
    }
  }
}

typedef struct
{
  uint32_t nid;
  uint32_t val;
} U32Enc;

static void sort_u32_pairs(U32Enc *pairs, uint8_t count)
{
  uint8_t i;
  uint8_t j;

  for (i = 0U; i + 1U < count; i++)
  {
    for (j = (uint8_t)(i + 1U); j < count; j++)
    {
      if (pairs[j].nid < pairs[i].nid)
      {
        U32Enc t = pairs[i];
        pairs[i] = pairs[j];
        pairs[j] = t;
      }
    }
  }
}

static void refresh_sorted_kv_view(void)
{
  uint16_t n = 0U;
  uint32_t ei;

  memset(g_sorted_ids, 0, sizeof(g_sorted_ids));

  if (BOOT_CANON_COUNT >= BOOT_METADATA_SORTED_CAP)
  {
    return;
  }

  for (uint32_t c = 0U; (c < BOOT_CANON_COUNT) && (n < BOOT_METADATA_SORTED_CAP); c++)
  {
    g_sorted_ids[n++] = k_boot_canon[c].node_id;
  }

  sort_extra_ids_by_node();
  for (ei = 0U; ei < (uint32_t)g_extra_count; ei++)
  {
    uint32_t id = g_extra_kv[ei].node_id;

    bool dup = false;
    for (uint16_t s = 0U; s < n; s++)
    {
      if (g_sorted_ids[s] == id)
      {
        dup = true;
        break;
      }
    }

    if (!dup && (n < BOOT_METADATA_SORTED_CAP))
    {
      g_sorted_ids[n++] = id;
    }
  }

  /* Insertion sort ascending by id while keeping deterministic order beyond ties */
  if (n > 1U)
  {
    for (uint16_t i = 1U; i < n; i++)
    {
      uint32_t key = g_sorted_ids[i];
      int j = (int)i - 1;
      while ((j >= 0) && (g_sorted_ids[(uint16_t)j] > key))
      {
        g_sorted_ids[(uint16_t)((uint16_t)j + 1U)] = g_sorted_ids[(uint16_t)j];
        j--;
      }

      g_sorted_ids[(uint16_t)((uint16_t)j + 1U)] = key;
    }
  }

  g_sorted_count = n;
}

static int encode_kv_blob_into_scratch(const BootKvRam *m_src, uint32_t *blob_len_out)
{
  BootKvRam meta = *m_src;
  U32Enc u32s[28];
  uint8_t nu32 = 0U;
  uint32_t ci;

  memset(s_blob_scratch, 0xFF, sizeof(s_blob_scratch));

  meta.magic = BOOT_METADATA_MAGIC;
  meta.version = BOOT_METADATA_VERSION;

  for (ci = 0U; ci < BOOT_CANON_COUNT; ci++)
  {
    const BootCanonEntry *e = &k_boot_canon[ci];

    if (e->in_u32_lane != 0U)
    {
      if (nu32 >= (uint8_t)(sizeof(u32s) / sizeof(u32s[0])))
      {
        return -1;
      }

      u32s[nu32].nid = e->node_id;
      u32s[nu32].val = canon_u32_lane_read(&meta, e);
      nu32++;
    }
  }

  sort_u32_pairs(u32s, nu32);

  /* Generic TLV payload starts at ofs 8; record_total at 0, generic_len at 4 */
  {
    uint32_t gen_ofs = 8U;
    uint32_t gen_start_ofs = gen_ofs;

    sort_extra_ids_by_node();

    for (ci = 0U; ci < BOOT_CANON_COUNT; ci++)
    {
      const BootCanonEntry *e = &k_boot_canon[ci];
      const uint8_t *src;

      if (e->in_tlv == 0U)
      {
        continue;
      }

      src = ((const uint8_t *)&meta) + e->field_offset;
      if (append_tlv_generic(s_blob_scratch, &gen_ofs, BOOT_SETTINGS_SCRATCH_MAX, e->node_id, src,
                             e->tlv_len) != 0)
      {
        return -1;
      }
    }

    {
      uint8_t ei;

      for (ei = 0U; ei < g_extra_count; ei++)
      {
        if (append_tlv_generic(s_blob_scratch, &gen_ofs, BOOT_SETTINGS_SCRATCH_MAX,
                               g_extra_kv[ei].node_id, g_extra_kv[ei].value, g_extra_kv[ei].length) !=
            0)
        {
          return -1;
        }
      }

      (void)ei;
    }

    {
      uint32_t generic_len = gen_ofs - gen_start_ofs;
      write_le_u32(s_blob_scratch + sizeof(uint32_t), generic_len);
      {
        uint32_t trailing = align_up_u32(gen_ofs, 4U);
        uint32_t uix;

        gen_ofs = trailing;
        write_le_u32(s_blob_scratch + gen_ofs, 0U); /* u8_pair_count */
        gen_ofs += sizeof(uint32_t);
        trailing = align_up_u32(gen_ofs, 4U);
        gen_ofs = trailing;

        write_le_u32(s_blob_scratch + gen_ofs, (uint32_t)nu32);
        gen_ofs += sizeof(uint32_t);

        for (uix = 0U; uix < nu32; uix++)
        {
          if ((gen_ofs + 8U) > BOOT_SETTINGS_SCRATCH_MAX - sizeof(uint32_t) - sizeof(uint32_t))
          {
            return -1;
          }

          write_le_u32(s_blob_scratch + gen_ofs, u32s[uix].nid);
          write_le_u32(s_blob_scratch + gen_ofs + 4U, u32s[uix].val);
          gen_ofs += 8U;
        }

        {
          uint32_t crc_field_offset = align_up_u32(gen_ofs, BOOT_SETTINGS_ALIGNMENT);

          memset(s_blob_scratch + gen_ofs, 0xFF, crc_field_offset - gen_ofs);

          if ((crc_field_offset + sizeof(uint32_t)) > BOOT_SETTINGS_SCRATCH_MAX)
          {
            return -1;
          }

          {
            uint32_t record_full_size = crc_field_offset + sizeof(uint32_t);

            write_le_u32(s_blob_scratch, record_full_size);
            write_le_u32(s_blob_scratch + crc_field_offset,
                         settings_crc_bytes(s_blob_scratch, crc_field_offset));
            *blob_len_out = record_full_size;
            return 0;
          }
        }
      }
    }
  }
}

static bool load_latest_blob_into_ram(uint32_t *newest_slot_out)
{
  uint32_t k = bitmap_count_used_slots();

  if (k == 0U)
  {
    return false;
  }

  {
    uint32_t newest_idx = k - 1U;
    uint32_t ptr = read_pointer_raw(newest_idx);

    const uint8_t *blob_start;
    uint32_t rec;
    uint32_t bound;

    if (!pointer_is_valid_reloc(ptr))
    {
      return false;
    }

    blob_start = (const uint8_t *)(uintptr_t)(BOOT_METADATA_BASE_ADDR + ptr);

    rec = read_le_u32(blob_start);

    if ((rec < 24U) || (BOOT_METADATA_BASE_ADDR + ptr + rec > BOOT_METADATA_LIMIT_ADDR))
    {
      return false;
    }

    bound = BOOT_METADATA_LIMIT_ADDR - (BOOT_METADATA_BASE_ADDR + ptr);

    if (parse_and_load_blob(blob_start, bound) != 0)
    {
      return false;
    }

    *newest_slot_out = newest_idx;
    g_active_slot = newest_idx;
    g_active_settings_phys_addr = (uint32_t)(uintptr_t)blob_start;
    g_active_settings_blob_size_bytes = rec;
    g_flash_bytes_used_cached = recompute_flash_high_water_relative();
    refresh_sorted_kv_view();
    return true;
  }
}

static int blob_append_physical(const uint32_t blob_len)
{
  uint32_t padded_len = flash_blob_footprint(blob_len);
  uint32_t phy_dst_rel;
  uint32_t phy_dst_phys;
  uint32_t slots_used_now;
  uint32_t heap_water;
  uint32_t slot_ix;

restart_after_erase_label:
  slots_used_now = bitmap_count_used_slots();

  heap_water =
      ((slots_used_now == 0U) ? BOOT_SETTINGS_INDEX_TOTAL_BYTES : recompute_flash_high_water_relative());

  if (slots_used_now >= BOOT_SETTINGS_INDEX_ENTRIES)
  {
    if (metadata_erase_sector() != 0)
    {
      return -1;
    }

    slots_used_now = 0U;
    heap_water = BOOT_SETTINGS_INDEX_TOTAL_BYTES;
  }

  if ((heap_water + padded_len > BOOT_METADATA_SIZE_BYTES) ||
      (heap_water + padded_len < heap_water))
  {
    if (metadata_erase_sector() != 0)
    {
      return -1;
    }

    goto restart_after_erase_label;
  }

  slot_ix = slots_used_now;
  phy_dst_rel = heap_water;
  phy_dst_phys = BOOT_METADATA_BASE_ADDR + phy_dst_rel;

  if (flash_program_bytes(phy_dst_phys, s_blob_scratch, padded_len) != 0)
  {
    return -1;
  }

  if (flash_program_word(ptr_table_phys_addr(slot_ix), phy_dst_rel) != 0)
  {
    return -1;
  }

  if (program_bitmap_commit_bit(slot_ix) != 0)
  {
    return -1;
  }

  g_active_slot = slot_ix;
  g_active_settings_phys_addr = phy_dst_phys;
  g_active_settings_blob_size_bytes = blob_len;
  g_flash_bytes_used_cached = recompute_flash_high_water_relative();
  refresh_sorted_kv_view();

  return 0;
}

static int metadata_physical_commit_incremented(void)
{
  BootKvRam work = g_kv_ram;
  uint32_t blob_len;

  /* Increment sequence committed for persisted record */
  if (work.sequence >= 0xFFFFFFFEUL)
  {
    /* rare wrap */
    work.sequence = 1U;
  }
  else
  {
    work.sequence++;
  }

  HAL_StatusTypeDef u = HAL_FLASH_Unlock();

  if (u != HAL_OK)
  {
    return -1;
  }

  if (encode_kv_blob_into_scratch(&work, &blob_len) != 0)
  {
    (void)HAL_FLASH_Lock();
    return -1;
  }

  /* Finalize CRC after record_total known - encode rewrote crc last */
  if (blob_append_physical(blob_len) != 0)
  {
    (void)HAL_FLASH_Lock();
    return -1;
  }

  (void)HAL_FLASH_Lock();
  g_kv_ram = work;

  refresh_sorted_kv_view();
  return 0;
}

void boot_metadata_init(void)
{
  uint32_t k = bitmap_count_used_slots();

  extras_clear_all();
  metadata_defaults_ram();
  g_active_slot = 0U;
  g_active_settings_phys_addr = 0U;
  g_active_settings_blob_size_bytes = 0U;
  g_flash_bytes_used_cached = BOOT_SETTINGS_INDEX_TOTAL_BYTES;

  if (k == 0U)
  {
    if (metadata_physical_commit_incremented() != 0)
    {
      refresh_sorted_kv_view();
      return;
    }

    refresh_sorted_kv_view();
    return;
  }

  {
    uint32_t newest_slot_dummy = bitmap_count_used_slots() - 1U;

    if (!load_latest_blob_into_ram(&newest_slot_dummy))
    {
      extras_clear_all();
      metadata_defaults_ram();

      (void)HAL_FLASH_Unlock();
      if (metadata_erase_sector() != 0)
      {
        (void)HAL_FLASH_Lock();
        refresh_sorted_kv_view();
        return;
      }
      (void)HAL_FLASH_Lock();

      if (metadata_physical_commit_incremented() != 0)
      {
        refresh_sorted_kv_view();
        return;
      }
    }
  }

  refresh_sorted_kv_view();
}

static uint8_t rail_mode_read_clamped(uint8_t raw_mode)
{
  return (raw_mode > 2U) ? BOOT_METADATA_DEFAULT_RAIL_MODE : raw_mode;
}

static void stored_kv_fmt_name(uint32_t node_id, char *name_out, size_t name_cap)
{
  if ((name_out == NULL) || (name_cap == 0U))
  {
    return;
  }

  name_out[0] = '\0';

  switch (node_id)
  {
  case BOOT_KV_SEQUENCE:
    (void)snprintf(name_out, name_cap, "seq");
    break;
  case BOOT_KV_APP_VALID:
    (void)snprintf(name_out, name_cap, "app_valid");
    break;
  case BOOT_KV_APP_DISABLED:
    (void)snprintf(name_out, name_cap, "app_disabled");
    break;
  case BOOT_KV_APP_VERSION:
    (void)snprintf(name_out, name_cap, "app_version");
    break;
  case BOOT_KV_FAULT_REASON:
    (void)snprintf(name_out, name_cap, "fault_reason");
    break;
  case BOOT_KV_FAULT_PC:
    (void)snprintf(name_out, name_cap, "fault_pc");
    break;
  case BOOT_KV_FAULT_LR:
    (void)snprintf(name_out, name_cap, "fault_lr");
    break;
  case BOOT_KV_IPV4_ADDR:
    (void)snprintf(name_out, name_cap, "ipv4");
    break;
  case BOOT_KV_IPV4_SUBNET:
    (void)snprintf(name_out, name_cap, "subnet");
    break;
  case BOOT_KV_IPV4_GW:
    (void)snprintf(name_out, name_cap, "gateway");
    break;
  case BOOT_KV_NET_MAC:
    (void)snprintf(name_out, name_cap, "mac");
    break;
  case BOOT_KV_HW_POLL_MS:
    (void)snprintf(name_out, name_cap, "poll_ms");
    break;
  case BOOT_KV_RAIL_A:
    (void)snprintf(name_out, name_cap, "rail_a");
    break;
  case BOOT_KV_RAIL_B:
    (void)snprintf(name_out, name_cap, "rail_b");
    break;
  case BOOT_KV_NET_DHCP:
    (void)snprintf(name_out, name_cap, "dhcp");
    break;
  default:
    if ((node_id & ~BOOT_KV_APP_STORAGE_MASK) == BOOT_KV_APP_STORAGE_BASE)
    {
      (void)snprintf(name_out, name_cap, "app_%08lx", (unsigned long)node_id);
    }
    else
    {
      (void)snprintf(name_out, name_cap, "kv%u", (unsigned int)node_id);
    }

    break;
  }
}

static void bytes_to_hex_trunc(const uint8_t *bytes, uint16_t byte_len, char *hex_out, size_t hex_cap)
{
  uint16_t i;

  if ((bytes == NULL) || (hex_out == NULL) || (hex_cap < 3U))
  {
    if (hex_out != NULL && hex_cap > 0U)
    {
      hex_out[0] = '\0';
    }

    return;
  }

  for (i = 0U; (i < byte_len) && ((((size_t)i * 2U) + 2U) <= (hex_cap - 1U)); i++)
  {
    (void)snprintf(hex_out + ((size_t)i * 2U), hex_cap - ((size_t)i * 2U), "%02X", (unsigned int)bytes[i]);
  }
}

static void stored_kv_fmt_value(uint32_t node_id, char *value_out, size_t value_cap)
{
  uint8_t buf[BOOT_KV_EXTRA_VALUE_MAX];
  uint16_t gotlen = 0U;

  if ((value_out == NULL) || (value_cap == 0U))
  {
    return;
  }

  value_out[0] = '\0';

  if ((node_id == BOOT_KV_IPV4_ADDR) || (node_id == BOOT_KV_IPV4_SUBNET) || (node_id == BOOT_KV_IPV4_GW))
  {
    if (boot_metadata_kv_read_bytes(node_id, buf, (uint16_t)sizeof(buf), &gotlen) == 0)
    {
      if (gotlen == 4U)
      {
        (void)snprintf(value_out, value_cap, "%u.%u.%u.%u", (unsigned int)buf[0], (unsigned int)buf[1],
                       (unsigned int)buf[2], (unsigned int)buf[3]);
      }

      else
      {
        bytes_to_hex_trunc(buf, gotlen, value_out, value_cap);
      }
    }

    return;
  }

  if (node_id == BOOT_KV_NET_MAC)
  {
    if (boot_metadata_kv_read_bytes(node_id, buf, (uint16_t)sizeof(buf), &gotlen) == 0)
    {
      if (gotlen == 6U)
      {
        (void)snprintf(value_out, value_cap, "%02X:%02X:%02X:%02X:%02X:%02X", (unsigned int)buf[0],
                       (unsigned int)buf[1], (unsigned int)buf[2], (unsigned int)buf[3],
                       (unsigned int)buf[4], (unsigned int)buf[5]);
      }

      else
      {
        bytes_to_hex_trunc(buf, gotlen, value_out, value_cap);
      }
    }

    return;
  }

  if ((node_id == BOOT_KV_RAIL_A) || (node_id == BOOT_KV_RAIL_B))
  {
    uint32_t v = 0U;

    if (boot_metadata_kv_read_u32(node_id, &v) == 0)
    {
      (void)snprintf(value_out, value_cap, "%lu", (unsigned long)v);
    }

    return;
  }

  if (node_id == BOOT_KV_NET_DHCP)
  {
    uint8_t d = 1U;
    uint16_t dn = 0U;

    if (boot_metadata_kv_read_bytes(BOOT_KV_NET_DHCP, &d, 1U, &dn) == 0)
    {
      if (dn == 1U)
      {
        (void)snprintf(value_out, value_cap, "%u", (unsigned int)d);
      }
    }

    return;
  }

  if (boot_metadata_kv_read_bytes(node_id, buf, (uint16_t)sizeof(buf), &gotlen) == 0)
  {
    bytes_to_hex_trunc(buf, gotlen, value_out, value_cap);
    return;
  }

  {
    uint32_t vu = 0U;

    if (boot_metadata_kv_read_u32(node_id, &vu) == 0)
    {
      (void)snprintf(value_out, value_cap, "%lu", (unsigned long)vu);
    }
  }
}

int boot_metadata_kv_read_u32(uint32_t node_id, uint32_t *out)
{
  const BootCanonEntry *e = canon_find_u32_lane_row(node_id);

  if ((out == NULL) || (e == NULL))
  {
    return -2;
  }

  *out = canon_u32_lane_read(&g_kv_ram, e);
  return 0;
}

int boot_metadata_kv_write_u32_commit(uint32_t node_id, uint32_t value)
{
  const BootCanonEntry *e = canon_find_u32_lane_row(node_id);

  if (e == NULL)
  {
    return -2;
  }

  if (canon_u32_lane_read(&g_kv_ram, e) == value)
  {
    return 0;
  }

  canon_u32_lane_write(&g_kv_ram, e, value);
  return metadata_physical_commit_incremented();
}

int boot_metadata_kv_read_bytes(uint32_t node_id, uint8_t *buf, uint16_t buf_cap, uint16_t *len_out)
{
  const BootCanonEntry *e_tlv = canon_find_tlv_row(node_id);

  if (len_out != NULL)
  {
    *len_out = 0U;
  }

  if ((buf == NULL) || (len_out == NULL))
  {
    return -2;
  }

  if (e_tlv != NULL)
  {
    if (buf_cap < e_tlv->tlv_len)
    {
      return -1;
    }

    memcpy(buf, ((const uint8_t *)&g_kv_ram) + e_tlv->field_offset, e_tlv->tlv_len);
    *len_out = e_tlv->tlv_len;
    return 0;
  }

  {
    const uint8_t *payload;
    uint16_t plen;

    if (extra_read_payload(node_id, &payload, &plen))
    {
      if (buf_cap < plen)
      {
        return -1;
      }

      if (plen > 0U)
      {
        memcpy(buf, payload, plen);
      }

      *len_out = plen;
      return 0;
    }
  }

  return -2;
}

int boot_metadata_kv_write_bytes_commit(uint32_t node_id, const uint8_t *data, uint16_t len)
{
  const BootCanonEntry *e_tlv = canon_find_tlv_row(node_id);
  uint8_t *slot;

  if ((len != 0U) && (data == NULL))
  {
    return -2;
  }

  if (e_tlv != NULL)
  {
    if (len != e_tlv->tlv_len)
    {
      return -2;
    }

    if (node_id == BOOT_KV_NET_DHCP)
    {
      if ((len != 1U) || (data[0] > 1U))
      {
        return -2;
      }
    }
    else if ((e_tlv->tlv_len == 1U) && (data[0] > 2U))
    {
      return -2;
    }

    slot = ((uint8_t *)&g_kv_ram) + e_tlv->field_offset;

    if (memcmp(slot, data, len) == 0)
    {
      return 0;
    }

    memcpy(slot, data, len);
    return metadata_physical_commit_incremented();
  }

  if (canon_find_u32_lane_row(node_id) != NULL)
  {
    return -2;
  }

  if (extra_try_put(node_id, data, len) != 0)
  {
    return -1;
  }

  return metadata_physical_commit_incremented();
}

bool boot_metadata_app_is_enabled(void)
{
  uint32_t disabled = 0U;

  if (boot_metadata_kv_read_u32(BOOT_KV_APP_DISABLED, &disabled) != 0)
  {
    return true;
  }

  return disabled == 0U;
}

bool boot_metadata_app_is_valid(void)
{
  uint32_t valid = 0U;

  if (boot_metadata_kv_read_u32(BOOT_KV_APP_VALID, &valid) != 0)
  {
    return false;
  }

  return valid != 0U;
}

int boot_metadata_set_app_valid(uint32_t app_version)
{
  g_kv_ram.app_valid = 1U;
  g_kv_ram.app_disabled = 0U;
  g_kv_ram.app_version = app_version;

  return metadata_physical_commit_incremented();
}

int boot_metadata_disable_app(BootAppFaultReason reason, uint32_t pc, uint32_t lr)
{
  g_kv_ram.app_valid = 0U;
  g_kv_ram.app_disabled = 1U;
  g_kv_ram.last_fault_reason = (uint32_t)reason;
  g_kv_ram.last_fault_pc = pc;
  g_kv_ram.last_fault_lr = lr;

  return metadata_physical_commit_incremented();
}

int boot_metadata_enable_app(void)
{
  g_kv_ram.app_disabled = 0U;
  g_kv_ram.app_valid = 1U;

  return metadata_physical_commit_incremented();
}

bool boot_metadata_ipv4_is_unusable_reserved(const uint8_t addr[4])
{
  static const uint8_t k_all_zero[4] = {0U, 0U, 0U, 0U};
  static const uint8_t k_all_one[4] = {255U, 255U, 255U, 255U};

  if (addr == NULL)
  {
    return true;
  }

  return (memcmp(addr, k_all_zero, 4U) == 0) || (memcmp(addr, k_all_one, 4U) == 0);
}

uint8_t boot_metadata_get_net_dhcp_enabled(void)
{
  return (g_kv_ram.net_dhcp_enabled != 0U) ? 1U : 0U;
}

int boot_metadata_set_net_dhcp_enabled(uint8_t enabled)
{
  uint8_t v = (enabled != 0U) ? 1U : 0U;

  if (g_kv_ram.net_dhcp_enabled == v)
  {
    return 0;
  }

  return boot_metadata_kv_write_bytes_commit(BOOT_KV_NET_DHCP, &v, 1U);
}

void boot_metadata_get_ipv4(uint8_t ip[4], uint8_t subnet[4], uint8_t gateway[4])
{
  uint16_t n;

  if (ip != NULL)
  {
    (void)boot_metadata_kv_read_bytes(BOOT_KV_IPV4_ADDR, ip, 4U, &n);
  }

  if (subnet != NULL)
  {
    (void)boot_metadata_kv_read_bytes(BOOT_KV_IPV4_SUBNET, subnet, 4U, &n);
  }

  if (gateway != NULL)
  {
    (void)boot_metadata_kv_read_bytes(BOOT_KV_IPV4_GW, gateway, 4U, &n);
  }
}

int boot_metadata_set_ipv4(const uint8_t ip[4], const uint8_t subnet[4], const uint8_t gateway[4])
{
  if ((ip == NULL) || (subnet == NULL) || (gateway == NULL))
  {
    return -1;
  }

  if ((memcmp(g_kv_ram.net_ipv4_addr, ip, 4U) == 0) &&
      (memcmp(g_kv_ram.net_ipv4_subnet, subnet, 4U) == 0) &&
      (memcmp(g_kv_ram.net_ipv4_gateway, gateway, 4U) == 0))
  {
    return 0;
  }

  memcpy(g_kv_ram.net_ipv4_addr, ip, 4U);
  memcpy(g_kv_ram.net_ipv4_subnet, subnet, 4U);
  memcpy(g_kv_ram.net_ipv4_gateway, gateway, 4U);

  return metadata_physical_commit_incremented();
}

void boot_metadata_get_mac(uint8_t mac[6])
{
  uint16_t n;

  if (mac != NULL)
  {
    (void)boot_metadata_kv_read_bytes(BOOT_KV_NET_MAC, mac, 6U, &n);
  }
}

int boot_metadata_set_mac(const uint8_t mac[6])
{
  if (mac == NULL)
  {
    return -1;
  }

  if (memcmp(g_kv_ram.net_mac, mac, 6U) == 0)
  {
    return 0;
  }

  memcpy(g_kv_ram.net_mac, mac, 6U);

  return metadata_physical_commit_incremented();
}

uint32_t boot_metadata_get_hardware_poll_period_ms(void)
{
  uint32_t ms = BOOT_METADATA_DEFAULT_HARDWARE_POLL_PERIOD_MS;

  if (boot_metadata_kv_read_u32(BOOT_KV_HW_POLL_MS, &ms) != 0)
  {
    ms = BOOT_METADATA_DEFAULT_HARDWARE_POLL_PERIOD_MS;
  }

  return ms;
}

int boot_metadata_set_hardware_poll_period_ms(uint32_t period_ms)
{
  uint32_t use = period_ms;

  if (use < BOOT_METADATA_DEFAULT_HARDWARE_POLL_PERIOD_MS)
  {
    use = BOOT_METADATA_DEFAULT_HARDWARE_POLL_PERIOD_MS;
  }

  if (use == g_kv_ram.hardware_poll_period_ms)
  {
    return 0;
  }

  g_kv_ram.hardware_poll_period_ms = use;

  return metadata_physical_commit_incremented();
}

uint8_t boot_metadata_get_rail_a_mode(void)
{
  uint32_t v = (uint32_t)BOOT_METADATA_DEFAULT_RAIL_MODE;

  if (boot_metadata_kv_read_u32(BOOT_KV_RAIL_A, &v) != 0)
  {
    v = BOOT_METADATA_DEFAULT_RAIL_MODE;
  }

  return rail_mode_read_clamped((uint8_t)(v & 0xFFU));
}

uint8_t boot_metadata_get_rail_b_mode(void)
{
  uint32_t v = (uint32_t)BOOT_METADATA_DEFAULT_RAIL_MODE;

  if (boot_metadata_kv_read_u32(BOOT_KV_RAIL_B, &v) != 0)
  {
    v = BOOT_METADATA_DEFAULT_RAIL_MODE;
  }

  return rail_mode_read_clamped((uint8_t)(v & 0xFFU));
}

int boot_metadata_set_rail_a_mode(uint8_t mode)
{
  uint8_t clipped = rail_mode_read_clamped(mode);

  if (mode != clipped)
  {
    return -1;
  }

  if (g_kv_ram.rail_a_mode == clipped)
  {
    return 0;
  }

  g_kv_ram.rail_a_mode = clipped;

  return metadata_physical_commit_incremented();
}

int boot_metadata_set_rail_b_mode(uint8_t mode)
{
  uint8_t clipped = rail_mode_read_clamped(mode);

  if (mode != clipped)
  {
    return -1;
  }

  if (g_kv_ram.rail_b_mode == clipped)
  {
    return 0;
  }

  g_kv_ram.rail_b_mode = clipped;

  return metadata_physical_commit_incremented();
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
  uint32_t u = boot_metadata_get_flash_bytes_used();

  if (u >= BOOT_METADATA_SIZE_BYTES)
  {
    return 0U;
  }

  return BOOT_METADATA_SIZE_BYTES - u;
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
  return g_sorted_count;
}

int boot_metadata_format_stored_kv(uint16_t sorted_index_one_based, char *name_out, size_t name_cap,
                                   char *value_out, size_t value_cap)
{
  uint32_t node_id;

  if ((sorted_index_one_based == 0U) || (sorted_index_one_based > g_sorted_count) || (name_out == NULL) ||
      (value_out == NULL))
  {
    return -1;
  }

  node_id = g_sorted_ids[(uint16_t)(sorted_index_one_based - 1U)];

  stored_kv_fmt_name(node_id, name_out, name_cap);
  stored_kv_fmt_value(node_id, value_out, value_cap);

  return 0;
}

int boot_metadata_storage_read_string_key(const char *key, void *data, size_t max_length, size_t *length_out)
{
  uint32_t node_id = metadata_app_storage_hash(key);
  uint16_t idx;
  uint16_t plen;
  uint8_t *dst = data;

  if (length_out != NULL)
  {
    *length_out = 0U;
  }

  if ((key == NULL) || (data == NULL) || (length_out == NULL))
  {
    return -1;
  }

  if (!extra_contains_node(node_id, &idx))
  {
    return 0;
  }

  plen = g_extra_kv[idx].length;

  if (max_length < (size_t)plen)
  {
    *length_out = (size_t)plen;
    return -1;
  }

  if (plen != 0U)
  {
    memcpy(dst, g_extra_kv[idx].value, plen);
  }

  /* optional terminating NUL within caller buffer span */
  if ((size_t)plen + 1U <= max_length)
  {
    dst[plen] = (uint8_t)'\0';
  }

  *length_out = (size_t)plen;
  return 0;
}

int boot_metadata_storage_write_string_key(const char *key, const void *data, size_t length)
{
  uint32_t node_id;

  if ((key == NULL) || ((length != 0U) && (data == NULL)) || (length > BOOT_KV_EXTRA_VALUE_MAX))
  {
    return -1;
  }

  node_id = metadata_app_storage_hash(key);

  {
    uint16_t idx;

    if (extra_contains_node(node_id, &idx))
    {
      if ((length == g_extra_kv[idx].length) && ((length == 0U) || (memcmp(data, g_extra_kv[idx].value, length) == 0)))
      {
        return 0;
      }
    }
  }

  if (extra_try_put(node_id, (const uint8_t *)data, (uint16_t)length) != 0)
  {
    return -1;
  }

  return metadata_physical_commit_incremented();
}