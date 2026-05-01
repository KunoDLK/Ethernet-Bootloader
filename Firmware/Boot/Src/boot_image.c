#include "boot_image.h"

#include "boot_memory_map.h"
#include "stm32f4xx_hal.h"

#include <string.h>

extern HASH_HandleTypeDef hhash;

static const uint8_t *stored_payload_base(void)
{
  return (const uint8_t *)(BOOT_APP_STORE_BASE_ADDR + sizeof(AppImageHeader));
}

BootImageStatus boot_image_read_header(AppImageHeader *header)
{
  if (header == 0)
  {
    return BOOT_IMAGE_BAD_HEADER;
  }

  memcpy(header, (const void *)BOOT_APP_STORE_BASE_ADDR, sizeof(AppImageHeader));
  return BOOT_IMAGE_OK;
}

static bool image_header_is_sane(const AppImageHeader *header)
{
  if ((header == 0) ||
      (header->magic != APP_IMAGE_MAGIC) ||
      (header->header_size != sizeof(AppImageHeader)) ||
      (header->abi_version != APP_IMAGE_ABI_VERSION) ||
      (header->digest_algorithm != APP_IMAGE_DIGEST_SHA1))
  {
    return false;
  }

  if ((header->image_size == 0U) || (header->exec_size == 0U) || (header->image_size > header->exec_size))
  {
    return false;
  }

  if (!boot_mem_is_app_store_range(BOOT_APP_STORE_BASE_ADDR, sizeof(AppImageHeader) + header->image_size))
  {
    return false;
  }

  if (!boot_mem_is_app_exec_range(header->exec_base, header->exec_size))
  {
    return false;
  }

  if ((header->entry_offset >= header->exec_size) ||
      (header->bss_offset > header->exec_size) ||
      (header->bss_size > (header->exec_size - header->bss_offset)))
  {
    return false;
  }

  return true;
}

BootImageStatus boot_image_validate(const AppImageHeader *header, bool verify_digest)
{
  if (!image_header_is_sane(header))
  {
    return BOOT_IMAGE_BAD_HEADER;
  }

  if (!verify_digest)
  {
    return BOOT_IMAGE_OK;
  }

  uint8_t digest[APP_IMAGE_SHA1_DIGEST_BYTES] = {0};
  if (HAL_HASH_SHA1_Start(&hhash, stored_payload_base(), header->image_size, digest, HAL_MAX_DELAY) != HAL_OK)
  {
    return BOOT_IMAGE_BAD_DIGEST;
  }

  if (memcmp(digest, header->sha1, APP_IMAGE_SHA1_DIGEST_BYTES) != 0)
  {
    return BOOT_IMAGE_BAD_DIGEST;
  }

  return BOOT_IMAGE_OK;
}

BootImageStatus boot_image_load_to_ccmram(const AppImageHeader *header)
{
  BootImageStatus status = boot_image_validate(header, true);
  if (status != BOOT_IMAGE_OK)
  {
    return status;
  }

  memset((void *)header->exec_base, 0, header->exec_size);
  memcpy((void *)header->exec_base, stored_payload_base(), header->image_size);

  if (header->bss_size != 0U)
  {
    memset((void *)(header->exec_base + header->bss_offset), 0, header->bss_size);
  }

  return BOOT_IMAGE_OK;
}

void *boot_image_entry_address(const AppImageHeader *header)
{
  if (!image_header_is_sane(header))
  {
    return 0;
  }

  return (void *)(header->exec_base + header->entry_offset);
}

void *boot_image_stop_address(const AppImageHeader *header)
{
  if (!image_header_is_sane(header) || (header->stop_offset == 0U))
  {
    return 0;
  }

  return (void *)(header->exec_base + header->stop_offset);
}
