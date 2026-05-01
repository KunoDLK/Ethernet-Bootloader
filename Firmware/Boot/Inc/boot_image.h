#ifndef BOOT_IMAGE_H
#define BOOT_IMAGE_H

#include "app_image.h"

#include <stdbool.h>

typedef enum
{
  BOOT_IMAGE_OK = 0,
  BOOT_IMAGE_BAD_HEADER = -1,
  BOOT_IMAGE_BAD_RANGE = -2,
  BOOT_IMAGE_BAD_DIGEST = -3,
  BOOT_IMAGE_UNSUPPORTED_ABI = -4,
} BootImageStatus;

BootImageStatus boot_image_read_header(AppImageHeader *header);
BootImageStatus boot_image_validate(const AppImageHeader *header, bool verify_digest);
BootImageStatus boot_image_load_to_ccmram(const AppImageHeader *header);
void *boot_image_entry_address(const AppImageHeader *header);
void *boot_image_stop_address(const AppImageHeader *header);

#endif /* BOOT_IMAGE_H */
