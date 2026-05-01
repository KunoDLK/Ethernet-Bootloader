#ifndef APP_IMAGE_H
#define APP_IMAGE_H

#include <stdint.h>

#define APP_IMAGE_MAGIC                 (0x41505031UL) /* "APP1" */
#define APP_IMAGE_ABI_VERSION           (1U)
#define APP_IMAGE_SHA1_DIGEST_BYTES     (20U)
#define APP_IMAGE_DIGEST_SHA1           (1U)

#define APP_IMAGE_FLAG_VALID            (1UL << 0)

typedef int (*AppStartFn)(const void *api);
typedef void (*AppStopFn)(void);

typedef struct __attribute__((packed))
{
  uint32_t magic;
  uint16_t header_size;
  uint16_t abi_version;
  uint32_t image_size;
  uint32_t exec_base;
  uint32_t exec_size;
  uint32_t entry_offset;
  uint32_t stop_offset;
  uint32_t bss_offset;
  uint32_t bss_size;
  uint32_t app_version;
  uint32_t flags;
  uint8_t digest_algorithm;
  uint8_t reserved[3];
  uint8_t sha1[APP_IMAGE_SHA1_DIGEST_BYTES];
} AppImageHeader;

#endif /* APP_IMAGE_H */
