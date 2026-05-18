#include "boot_iap_payload.h"

#include <string.h>

int boot_iap_payload_read_header(BootIapPayloadHeader *header_out)
{
  if (header_out == 0)
  {
    return -1;
  }

  memcpy(header_out, (const void *)BOOT_BOOTLOADER_PAYLOAD_BASE_ADDR, sizeof(*header_out));
  return 0;
}

int boot_iap_payload_validate_header(const BootIapPayloadHeader *header)
{
  if ((header == 0) ||
      (header->magic != BOOT_IAP_PAYLOAD_MAGIC) ||
      (header->image_length == 0U) ||
      (header->image_length > BOOT_IAP_PAYLOAD_IMAGE_MAX_BYTES))
  {
    return -1;
  }

  if (!boot_mem_is_bootloader_payload_range(BOOT_IAP_PAYLOAD_IMAGE_BASE_ADDR, header->image_length))
  {
    return -1;
  }

  return 0;
}

const uint8_t *boot_iap_payload_image_ptr(const BootIapPayloadHeader *header)
{
  if (boot_iap_payload_validate_header(header) != 0)
  {
    return 0;
  }

  return (const uint8_t *)BOOT_IAP_PAYLOAD_IMAGE_BASE_ADDR;
}
