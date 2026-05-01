#include "boot_metadata.h"

#include "boot_memory_map.h"
#include "stm32f4xx_hal.h"

#include <string.h>

static BootMetadata g_metadata;

static uint32_t metadata_crc32(const BootMetadata *metadata)
{
  const uint32_t *words = (const uint32_t *)metadata;
  uint32_t crc = 0xFFFFFFFFUL;

  for (uint32_t i = 0U; i < ((sizeof(BootMetadata) / sizeof(uint32_t)) - 1U); i++)
  {
    crc ^= words[i];
    for (uint32_t bit = 0U; bit < 32U; bit++)
    {
      crc = (crc >> 1U) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
    }
  }

  return ~crc;
}

static bool metadata_is_valid(const BootMetadata *metadata)
{
  return (metadata->magic == BOOT_METADATA_MAGIC) &&
         (metadata->version == BOOT_METADATA_VERSION) &&
         (metadata->crc32 == metadata_crc32(metadata));
}

static void metadata_set_defaults(void)
{
  memset(&g_metadata, 0, sizeof(g_metadata));
  g_metadata.magic = BOOT_METADATA_MAGIC;
  g_metadata.version = BOOT_METADATA_VERSION;
  g_metadata.crc32 = metadata_crc32(&g_metadata);
}

static int metadata_commit(void)
{
  BootMetadata next = g_metadata;
  next.sequence++;
  next.crc32 = metadata_crc32(&next);

  HAL_StatusTypeDef status = HAL_FLASH_Unlock();
  if (status != HAL_OK)
  {
    return -1;
  }

  FLASH_EraseInitTypeDef erase = {0};
  uint32_t sector_error = 0U;
  erase.TypeErase = FLASH_TYPEERASE_SECTORS;
  erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
  erase.Sector = FLASH_SECTOR_11;
  erase.NbSectors = 1U;

  status = HAL_FLASHEx_Erase(&erase, &sector_error);
  if (status == HAL_OK)
  {
    const uint32_t *src = (const uint32_t *)&next;
    uint32_t dst = BOOT_METADATA_BASE_ADDR;
    for (uint32_t i = 0U; i < (sizeof(BootMetadata) / sizeof(uint32_t)); i++)
    {
      status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, dst, src[i]);
      if (status != HAL_OK)
      {
        break;
      }
      dst += sizeof(uint32_t);
    }
  }

  (void)HAL_FLASH_Lock();

  if (status == HAL_OK)
  {
    g_metadata = next;
    return 0;
  }

  return -1;
}

void boot_metadata_init(void)
{
  const BootMetadata *stored = (const BootMetadata *)BOOT_METADATA_BASE_ADDR;
  if (metadata_is_valid(stored))
  {
    g_metadata = *stored;
  }
  else
  {
    metadata_set_defaults();
  }
}

const BootMetadata *boot_metadata_get(void)
{
  return &g_metadata;
}

bool boot_metadata_app_is_enabled(void)
{
  return g_metadata.app_disabled == 0U;
}

bool boot_metadata_app_is_valid(void)
{
  return g_metadata.app_valid != 0U;
}

int boot_metadata_set_app_valid(uint32_t app_version)
{
  g_metadata.app_valid = 1U;
  g_metadata.app_disabled = 0U;
  g_metadata.app_version = app_version;
  g_metadata.last_fault_reason = BOOT_APP_FAULT_NONE;
  g_metadata.last_fault_pc = 0U;
  g_metadata.last_fault_lr = 0U;
  return metadata_commit();
}

int boot_metadata_disable_app(BootAppFaultReason reason, uint32_t pc, uint32_t lr)
{
  g_metadata.app_disabled = 1U;
  g_metadata.last_fault_reason = (uint32_t)reason;
  g_metadata.last_fault_pc = pc;
  g_metadata.last_fault_lr = lr;
  return metadata_commit();
}

int boot_metadata_enable_app(void)
{
  g_metadata.app_disabled = 0U;
  g_metadata.last_fault_reason = BOOT_APP_FAULT_NONE;
  g_metadata.last_fault_pc = 0U;
  g_metadata.last_fault_lr = 0U;
  return metadata_commit();
}
