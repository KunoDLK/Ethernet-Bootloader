#ifndef BOOT_METADATA_H
#define BOOT_METADATA_H

#include "boot_memory_map.h"

#include <stdbool.h>
#include <stdint.h>

#define BOOT_METADATA_MAGIC             (0x4D455441UL) /* "META" */
#define BOOT_METADATA_VERSION           (5U)
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
  uint8_t reserved_rails[2];
  uint32_t crc32;
} BootMetadata;

/* Flash slot stride and slot count: derived from sizeof(BootMetadata) and sector size. */
#define BOOT_METADATA_SLOT_ALIGN        (8U)
#define BOOT_METADATA_SLOT_STRIDE_BYTES                                      \
  ((((uint32_t)sizeof(BootMetadata) + BOOT_METADATA_SLOT_ALIGN - 1U) /       \
    BOOT_METADATA_SLOT_ALIGN) *                                            \
   BOOT_METADATA_SLOT_ALIGN)

#define BOOT_METADATA_BITMAP_BYTES_FOR_SLOTS(n)                              \
  ((((uint32_t)(n) + 7U) / 8U + 3U) & ~3U)

/* Lower bound on slots so n*stride + rounded-up bitmap always fits the sector. */
#define BOOT_METADATA_MAX_SLOTS                                              \
  ((8U * (uint32_t)(BOOT_METADATA_SIZE_BYTES)) /                             \
   (8U * BOOT_METADATA_SLOT_STRIDE_BYTES + 1U))

_Static_assert(sizeof(BootMetadata) <= BOOT_METADATA_SLOT_STRIDE_BYTES,
               "BootMetadata larger than slot stride");
_Static_assert(BOOT_METADATA_SLOT_STRIDE_BYTES % 4U == 0U,
               "slot stride must be multiple of flash word size");
_Static_assert(BOOT_METADATA_MAX_SLOTS >= 1U, "metadata sector too small");
_Static_assert(
    ((unsigned long long)BOOT_METADATA_MAX_SLOTS *                           \
     (unsigned long long)BOOT_METADATA_SLOT_STRIDE_BYTES) +                 \
            (unsigned long long)BOOT_METADATA_BITMAP_BYTES_FOR_SLOTS(        \
                BOOT_METADATA_MAX_SLOTS) <=                                 \
        (unsigned long long)(BOOT_METADATA_SIZE_BYTES),
    "metadata bitmap + slots exceed sector");

void boot_metadata_init(void);
const BootMetadata *boot_metadata_get(void);
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
uint32_t boot_metadata_get_total_settings_slots(void);

#endif /* BOOT_METADATA_H */
