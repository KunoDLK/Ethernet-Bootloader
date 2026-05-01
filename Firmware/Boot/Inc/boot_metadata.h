#ifndef BOOT_METADATA_H
#define BOOT_METADATA_H

#include <stdbool.h>
#include <stdint.h>

#define BOOT_METADATA_MAGIC             (0x4D455441UL) /* "META" */
#define BOOT_METADATA_VERSION           (1U)

typedef enum
{
  BOOT_APP_FAULT_NONE = 0,
  BOOT_APP_FAULT_MEMMANAGE = 1,
  BOOT_APP_FAULT_BUS = 2,
  BOOT_APP_FAULT_USAGE = 3,
  BOOT_APP_FAULT_HARD = 4,
} BootAppFaultReason;

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
  uint32_t crc32;
} BootMetadata;

void boot_metadata_init(void);
const BootMetadata *boot_metadata_get(void);
bool boot_metadata_app_is_enabled(void);
bool boot_metadata_app_is_valid(void);
int boot_metadata_set_app_valid(uint32_t app_version);
int boot_metadata_disable_app(BootAppFaultReason reason, uint32_t pc, uint32_t lr);
int boot_metadata_enable_app(void);

#endif /* BOOT_METADATA_H */
