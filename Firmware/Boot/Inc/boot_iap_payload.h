#ifndef BOOT_IAP_PAYLOAD_H
#define BOOT_IAP_PAYLOAD_H

#include <stddef.h>
#include <stdint.h>

#include "boot_memory_map.h"

#define BOOT_IAP_PAYLOAD_MAGIC            (0x54534C42UL) /* "BLST" */
#define BOOT_IAP_PAYLOAD_HEADER_SIZE      (32U)
#define BOOT_IAP_PAYLOAD_IMAGE_BASE_ADDR  (BOOT_BOOTLOADER_PAYLOAD_BASE_ADDR + BOOT_IAP_PAYLOAD_HEADER_SIZE)
#define BOOT_IAP_PAYLOAD_IMAGE_MAX_BYTES  (BOOT_BOOTLOADER_PAYLOAD_SIZE_BYTES - BOOT_IAP_PAYLOAD_HEADER_SIZE)
#define BOOT_IAP_PAYLOAD_SHA1_BYTES       (20U)

typedef struct __attribute__((packed))
{
  uint32_t magic;
  uint32_t image_length;
  uint8_t sha1[BOOT_IAP_PAYLOAD_SHA1_BYTES];
  uint32_t reserved;
} BootIapPayloadHeader;

int boot_iap_payload_read_header(BootIapPayloadHeader *header_out);
int boot_iap_payload_validate_header(const BootIapPayloadHeader *header);
const uint8_t *boot_iap_payload_image_ptr(const BootIapPayloadHeader *header);

#endif /* BOOT_IAP_PAYLOAD_H */
