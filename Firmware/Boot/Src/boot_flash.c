#include "boot_flash.h"

#include "boot_memory_map.h"
#include "cmsis_os.h"
#include "stm32f4xx_hal.h"

#include <string.h>

#define BOOT_APP_FIRST_FLASH_SECTOR  FLASH_SECTOR_6
#define BOOT_APP_FLASH_SECTOR_COUNT  5U
#define BOOT_FLASH_TIMEOUT_VALUE     50000U

static uint8_t g_app_storage_programming_active;

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

static HAL_StatusTypeDef flash_program_word(uint32_t address, const uint8_t bytes[4])
{
  uint32_t word = 0U;
  memcpy(&word, bytes, sizeof(word));
  return HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address, word);
}

static HAL_StatusTypeDef flash_program_words(uint32_t address, const uint8_t *bytes,
                                             size_t word_count)
{
  HAL_StatusTypeDef status = FLASH_WaitForLastOperation(BOOT_FLASH_TIMEOUT_VALUE);
  if (status != HAL_OK)
  {
    return status;
  }

  CLEAR_BIT(FLASH->CR, FLASH_CR_PSIZE);
  FLASH->CR |= FLASH_PSIZE_WORD;
  FLASH->CR |= FLASH_CR_PG;

  while (word_count != 0U)
  {
    uint32_t word = 0U;
    memcpy(&word, bytes, sizeof(word));

    *(__IO uint32_t *)address = word;
    status = FLASH_WaitForLastOperation(BOOT_FLASH_TIMEOUT_VALUE);
    if (status != HAL_OK)
    {
      break;
    }

    address += sizeof(word);
    bytes += sizeof(word);
    word_count--;
  }

  FLASH->CR &= (~FLASH_CR_PG);
  return status;
}

int boot_flash_begin_app_storage_programming(void)
{
  if (g_app_storage_programming_active != 0U)
  {
    return 0;
  }

  if (HAL_FLASH_Unlock() != HAL_OK)
  {
    return -1;
  }

  g_app_storage_programming_active = 1U;
  return 0;
}

void boot_flash_end_app_storage_programming(void)
{
  if (g_app_storage_programming_active == 0U)
  {
    return;
  }

  g_app_storage_programming_active = 0U;
  (void)HAL_FLASH_Lock();
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
  erase.NbSectors = 1U;

  for (uint32_t sector_index = 0U; sector_index < BOOT_APP_FLASH_SECTOR_COUNT; sector_index++)
  {
    erase.Sector = BOOT_APP_FIRST_FLASH_SECTOR + sector_index;
    sector_error = 0U;

    status = HAL_FLASHEx_Erase(&erase, &sector_error);
    if (status != HAL_OK)
    {
      break;
    }

    if ((sector_index + 1U) < BOOT_APP_FLASH_SECTOR_COUNT)
    {
      osDelay(1U);
    }
  }
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

  if (g_app_storage_programming_active == 0U)
  {
    return -1;
  }

  HAL_StatusTypeDef status = HAL_OK;
  const uint8_t *bytes = (const uint8_t *)data;
  size_t remaining = length;

  const uint32_t leading_word_offset = address & 0x3UL;
  if (leading_word_offset != 0U)
  {
    uint32_t word = 0xFFFFFFFFUL;
    const uint32_t aligned_address = address - leading_word_offset;
    const size_t copy_len = ((4U - leading_word_offset) < remaining) ?
                            (4U - leading_word_offset) : remaining;

    memcpy(&word, (const void *)aligned_address, sizeof(word));
    memcpy(((uint8_t *)&word) + leading_word_offset, bytes, copy_len);

    status = flash_program_word(aligned_address, (const uint8_t *)&word);
    if (status != HAL_OK)
    {
      return -1;
    }

    bytes += copy_len;
    address += copy_len;
    remaining -= copy_len;
  }

  const size_t word_count = remaining / sizeof(uint32_t);
  if (word_count != 0U)
  {
    status = flash_program_words(address, bytes, word_count);
    if (status != HAL_OK)
    {
      return -1;
    }

    const size_t programmed_bytes = word_count * sizeof(uint32_t);
    bytes += programmed_bytes;
    address += programmed_bytes;
    remaining -= programmed_bytes;
  }

  if (remaining != 0U)
  {
    uint32_t word = 0xFFFFFFFFUL;

    memcpy(&word, (const void *)address, sizeof(word));
    memcpy(&word, bytes, remaining);

    status = flash_program_word(address, (const uint8_t *)&word);
  }

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
