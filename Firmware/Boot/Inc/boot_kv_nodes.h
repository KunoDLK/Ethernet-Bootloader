#ifndef BOOT_KV_NODES_H
#define BOOT_KV_NODES_H

#include <stdint.h>

/* Stored in KV blob (sorted by id for deterministic encoding); app range for AppApiStorage. */
#define BOOT_KV_SEQUENCE               (1U)
#define BOOT_KV_APP_VALID              (2U)
#define BOOT_KV_APP_DISABLED           (3U)
#define BOOT_KV_APP_VERSION            (4U)
#define BOOT_KV_FAULT_REASON           (5U)
#define BOOT_KV_FAULT_PC               (6U)
#define BOOT_KV_FAULT_LR               (7U)
#define BOOT_KV_IPV4_ADDR              (8U)
#define BOOT_KV_IPV4_SUBNET            (9U)
#define BOOT_KV_IPV4_GW                (10U)
#define BOOT_KV_NET_MAC                (11U)
#define BOOT_KV_HW_POLL_MS             (12U)
#define BOOT_KV_RAIL_A                 (13U)
#define BOOT_KV_RAIL_B                 (14U)

#define BOOT_KV_APP_STORAGE_BASE       (0xA0000000UL)
#define BOOT_KV_APP_STORAGE_MASK       (0x07FFFFFFUL)

#endif /* BOOT_KV_NODES_H */
