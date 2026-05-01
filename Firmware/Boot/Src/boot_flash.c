#include "boot_flash.h"

#include "boot_memory_map.h"
#include "stm32f4xx_hal.h"

#include <string.h>

static int app_offset_to_addr(uint32_t offset, size_t length, uint32_t *address)
{
  if (length == 0U)
  {
    return -1;
  }

  const uint32_t addr = BOOT_APP_STORE_BASE_ADDR + offset;
  if ((addr < BOOT_APP_STORE_BASE_ADDR) || !boot_mem_is_app_store_range(addr, (uint32_t)length))
  {
    return -1;
  }

  *address = addr;
  return 0;
}

int boot_flash_erase_app_storage(void)
{
  HAL_StatusTypeDef status = HAL_FLASH_Unlock();
  if (status != HAL_OK)
  {
    return -1;
  }

  FLASH_EraseInitTypeDef erase = {0};
  uint32_t sector_error = 0U;
  erase.TypeErase = FLASH_TYPEERASE_SECTORS;
  erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
  erase.Sector = FLASH_SECTOR_6;
  erase.NbSectors = 5U;

  status = HAL_FLASHEx_Erase(&erase, &sector_error);
  (void)HAL_FLASH_Lock();

  return (status == HAL_OK) ? 0 : -1;
}

int boot_flash_write_app_storage(uint32_t offset, const void *data, size_t length)
{
  if (data == 0)
  {
    return -1;
  }

  uint32_t address = 0U;
  if (app_offset_to_addr(offset, length, &address) != 0)
  {
    return -1;
  }

  HAL_StatusTypeDef status = HAL_FLASH_Unlock();
  if (status != HAL_OK)
  {
    return -1;
  }

  const uint8_t *bytes = (const uint8_t *)data;
  size_t remaining = length;
  while (remaining != 0U)
  {
    uint32_t word = 0xFFFFFFFFUL;
    const uint32_t word_offset = address & 0x3UL;
    const uint32_t aligned_address = address - word_offset;
    const size_t copy_len = ((4U - word_offset) < remaining) ? (4U - word_offset) : remaining;

    memcpy(&word, (const void *)aligned_address, sizeof(word));
    memcpy(((uint8_t *)&word) + word_offset, bytes, copy_len);

    status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, aligned_address, word);
    if (status != HAL_OK)
    {
      break;
    }

    bytes += copy_len;
    address += copy_len;
    remaining -= copy_len;
  }

  (void)HAL_FLASH_Lock();
  return (status == HAL_OK) ? 0 : -1;
}

int boot_flash_read_app_storage(uint32_t offset, void *data, size_t length)
{
  if (data == 0)
  {
    return -1;
  }

  uint32_t address = 0U;
  if (app_offset_to_addr(offset, length, &address) != 0)
  {
    return -1;
  }

  memcpy(data, (const void *)address, length);
  return 0;
}
