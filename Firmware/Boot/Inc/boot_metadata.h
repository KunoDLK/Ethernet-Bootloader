#ifndef BOOT_METADATA_H
#define BOOT_METADATA_H

#include "boot_kv_nodes.h"
#include "boot_memory_map.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOT_METADATA_MAGIC             (0x4B565354UL) /* "KVST" */
#define BOOT_METADATA_VERSION           (1U)
#define BOOT_METADATA_DEFAULT_HARDWARE_POLL_PERIOD_MS (10U)
#define BOOT_METADATA_DEFAULT_RAIL_MODE (0U) /* 0=Disabled, 1=Enabled, 2=Follow Estop */
#define BOOT_METADATA_STORED_KV_MAX     (96U)
#define BOOT_METADATA_KV_VALUE_MAX      (236U)

typedef struct
{
  const uint8_t *value;
  uint16_t value_len;
} BootMetadataValueView;

typedef struct
{
  uint32_t key;
  BootMetadataValueView value;
} BootMetadataEntryView;

/* Flash index geometry: bitmap bits + pointers to variable-length blobs. */
#define BOOT_SETTINGS_INDEX_ENTRIES     (500U)
#define BOOT_SETTINGS_BITMAP_BYTES                                                          \
  ((((uint32_t)BOOT_SETTINGS_INDEX_ENTRIES + 7U) / 8U + 3U) & ~3U)
#define BOOT_SETTINGS_POINTER_BYTES     ((uint32_t)BOOT_SETTINGS_INDEX_ENTRIES * sizeof(uint32_t))
#define BOOT_SETTINGS_INDEX_TOTAL_BYTES                                                          \
  (BOOT_SETTINGS_BITMAP_BYTES + BOOT_SETTINGS_POINTER_BYTES)

_Static_assert(((size_t)(BOOT_METADATA_BASE_ADDR) + BOOT_SETTINGS_INDEX_TOTAL_BYTES) <=                     \
                   (size_t)(BOOT_METADATA_LIMIT_ADDR),                                           \
               "flash index overlaps metadata sector boundary");

int boot_metadata_initialize(void);
int boot_metadata_clear_sector_and_reset_store(void);
int boot_metadata_get(uint32_t key, BootMetadataValueView *value_out);
int boot_metadata_set(uint32_t key, const uint8_t *value, uint16_t value_len);
int boot_metadata_set_many(const BootMetadataEntryView *entries, uint16_t entry_count);
int boot_metadata_remove(uint32_t key);
int boot_metadata_save_to_flash(void);
uint16_t boot_metadata_count(void);
int boot_metadata_get_by_index(uint16_t index, BootMetadataEntryView *entry_out);
bool boot_metadata_validate_current_store(void);

uint32_t boot_metadata_get_current_settings_slot(void);
uint32_t boot_metadata_get_flash_bytes_used(void);
uint32_t boot_metadata_get_flash_bytes_remaining(void);
uint32_t boot_metadata_get_current_settings_object_phys_addr(void);
uint32_t boot_metadata_get_current_settings_object_size_bytes(void);

/* Backwards-compatible debug enumeration wrapper around the raw KV store. */
uint16_t boot_metadata_stored_kv_count(void);
int boot_metadata_read_stored_kv_raw(uint16_t index_one_based, uint32_t *key_out,
                                     uint8_t *value_out, uint16_t value_cap, uint16_t *value_len_out);

/* AppApiStorage string-key adapter; callers own data format. */
int boot_metadata_storage_read_string_key(const char *key, void *data, size_t max_length,
                                          size_t *length_out);
int boot_metadata_storage_write_string_key(const char *key, const void *data, size_t length);

#endif /* BOOT_METADATA_H */
