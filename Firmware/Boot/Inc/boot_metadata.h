#ifndef BOOT_METADATA_H
#define BOOT_METADATA_H

#include "boot_kv_nodes.h"
#include "boot_memory_map.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOT_METADATA_MAGIC             (0x4D455441UL) /* "META" RAM image only */
#define BOOT_METADATA_VERSION           (6U)
#define BOOT_METADATA_DEFAULT_HARDWARE_POLL_PERIOD_MS (10U)
#define BOOT_METADATA_DEFAULT_RAIL_MODE (0U) /* 0=Disabled, 1=Enabled, 2=Follow Estop */

typedef enum
{
  BOOT_APP_FAULT_NONE = 0,
  BOOT_APP_FAULT_MEMMANAGE = 1,
  BOOT_APP_FAULT_BUS = 2,
  BOOT_APP_FAULT_USAGE = 3,
  BOOT_APP_FAULT_HARD = 4,
} BootAppFaultReason;

/* Persisted configuration is keyed by BOOT_KV_* node ids only; consume via boot_metadata_kv_* and helpers below. */

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

void boot_metadata_init(void);
bool boot_metadata_app_is_enabled(void);
bool boot_metadata_app_is_valid(void);
int boot_metadata_set_app_valid(uint32_t app_version);
int boot_metadata_disable_app(BootAppFaultReason reason, uint32_t pc, uint32_t lr);
int boot_metadata_enable_app(void);
void boot_metadata_get_ipv4(uint8_t ip[4], uint8_t subnet[4], uint8_t gateway[4]);
int boot_metadata_set_ipv4(const uint8_t ip[4], const uint8_t subnet[4], const uint8_t gateway[4]);
void boot_metadata_get_mac(uint8_t mac[6]);
int boot_metadata_set_mac(const uint8_t mac[6]);
uint32_t boot_metadata_get_hardware_poll_period_ms(void);
int boot_metadata_set_hardware_poll_period_ms(uint32_t period_ms);
uint8_t boot_metadata_get_rail_a_mode(void);
uint8_t boot_metadata_get_rail_b_mode(void);
int boot_metadata_set_rail_a_mode(uint8_t mode);
int boot_metadata_set_rail_b_mode(uint8_t mode);
uint32_t boot_metadata_get_current_settings_slot(void);
uint32_t boot_metadata_get_flash_bytes_used(void);
uint32_t boot_metadata_get_flash_bytes_remaining(void);

/** Absolute flash address where the loaded KV/settings record begins; 0 if unknown. */
uint32_t boot_metadata_get_current_settings_object_phys_addr(void);
/** `record_total` size in bytes from the KV blob header for the loaded record; 0 if unknown. */
uint32_t boot_metadata_get_current_settings_object_size_bytes(void);

/* Debug / device tree: canonical + extra stored KVs merged, sorted ascending by node id. */
uint16_t boot_metadata_stored_kv_count(void);
/** sorted_index is 1-based (matches device-tree child ids). Returns 0 on success, -1 on range. */
int boot_metadata_format_stored_kv(uint16_t sorted_index_one_based, char *name_out, size_t name_cap,
                                   char *value_out, size_t value_cap);

/** AppApiStorage: key is NUL-terminated ASCII; persists in generic TLV under BOOT_KV_APP_STORAGE_BASE|^hash. */
int boot_metadata_storage_read_string_key(const char *key, void *data, size_t max_length,
                                          size_t *length_out);
int boot_metadata_storage_write_string_key(const char *key, const void *data, size_t length);

/**
 * Canonical settings by KV node id. Committed helpers flush to flash.
 * Returns 0 on success, -2 if node/type mismatch, -1 if commit failed or extras full.
 */
int boot_metadata_kv_read_u32(uint32_t node_id, uint32_t *out);
int boot_metadata_kv_write_u32_commit(uint32_t node_id, uint32_t value);
int boot_metadata_kv_read_bytes(uint32_t node_id, uint8_t *buf, uint16_t buf_cap, uint16_t *len_out);
int boot_metadata_kv_write_bytes_commit(uint32_t node_id, const uint8_t *data, uint16_t len);

#endif /* BOOT_METADATA_H */
