#include "boot_flash_resident.h"

#include "boot_memory_map.h"
#include "boot_sha1.h"

#include "stm32f4xx.h"
#include "stm32f4xx_hal_def.h"

#include <string.h>

#define BOOT_FLASH_TIMEOUT_LOOPS   (5000000U)

static __RAM_FUNC int flash_wait_ready(void)
{
  uint32_t loops = 0U;
  while ((FLASH->SR & FLASH_SR_BSY) != 0U)
  {
    if (++loops > BOOT_FLASH_TIMEOUT_LOOPS)
    {
      return -1;
    }
  }

  if ((FLASH->SR & (FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_PGPERR | FLASH_SR_PGSERR)) != 0U)
  {
    return -1;
  }

  return 0;
}

static __RAM_FUNC void flash_clear_status(void)
{
  FLASH->SR = FLASH_SR_EOP | FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_PGPERR | FLASH_SR_PGSERR;
}

static __RAM_FUNC int flash_unlock(void)
{
  if ((FLASH->CR & FLASH_CR_LOCK) == 0U)
  {
    return 0;
  }

  FLASH->KEYR = 0x45670123U;
  FLASH->KEYR = 0xCDEF89ABU;
  return ((FLASH->CR & FLASH_CR_LOCK) == 0U) ? 0 : -1;
}

static __RAM_FUNC void flash_lock(void)
{
  FLASH->CR |= FLASH_CR_LOCK;
}

static __RAM_FUNC int flash_erase_sector(uint32_t sector)
{
  if (flash_wait_ready() != 0)
  {
    return -1;
  }

  flash_clear_status();
  FLASH->CR &= ~(FLASH_CR_SNB | FLASH_CR_PSIZE | FLASH_CR_SER | FLASH_CR_PG);
  FLASH->CR |= FLASH_PSIZE_WORD;
  FLASH->CR |= FLASH_CR_SER | (sector << FLASH_CR_SNB_Pos);
  FLASH->CR |= FLASH_CR_STRT;

  if (flash_wait_ready() != 0)
  {
    FLASH->CR &= ~FLASH_CR_SER;
    return -1;
  }

  FLASH->CR &= ~FLASH_CR_SER;
  flash_clear_status();
  return 0;
}

static __RAM_FUNC int flash_program_word(uint32_t address, uint32_t word)
{
  if (flash_wait_ready() != 0)
  {
    return -1;
  }

  flash_clear_status();
  FLASH->CR &= ~(FLASH_CR_PSIZE | FLASH_CR_SER | FLASH_CR_SNB);
  FLASH->CR |= FLASH_PSIZE_WORD | FLASH_CR_PG;

  *(__IO uint32_t *)address = word;
  if (flash_wait_ready() != 0)
  {
    FLASH->CR &= ~FLASH_CR_PG;
    return -1;
  }

  FLASH->CR &= ~FLASH_CR_PG;
  flash_clear_status();
  return 0;
}

int boot_flash_resident_erase_sectors_0_to_5(void)
{
  if (flash_unlock() != 0)
  {
    return -1;
  }

  for (uint32_t sector = 0U; sector <= 5U; sector++)
  {
    if (flash_erase_sector(sector) != 0)
    {
      flash_lock();
      return -1;
    }
  }

  flash_lock();
  return 0;
}

int boot_flash_resident_program(uint32_t destination, const uint8_t *source, size_t length)
{
  if ((source == 0) || (length == 0U))
  {
    return -1;
  }

  if (!boot_mem_is_resident_range(destination, (uint32_t)length))
  {
    return -1;
  }

  if (flash_unlock() != 0)
  {
    return -1;
  }

  uint32_t address = destination;
  const uint8_t *bytes = source;
  size_t remaining = length;

  const uint32_t leading_offset = address & 0x3UL;
  if (leading_offset != 0U)
  {
    const uint32_t aligned = address - leading_offset;
    const size_t copy_len = ((4U - leading_offset) < remaining) ? (4U - leading_offset) : remaining;
    uint32_t word = *(const uint32_t *)aligned;
    memcpy(((uint8_t *)&word) + leading_offset, bytes, copy_len);
    if (flash_program_word(aligned, word) != 0)
    {
      flash_lock();
      return -1;
    }
    address += copy_len;
    bytes += copy_len;
    remaining -= copy_len;
  }

  while (remaining >= sizeof(uint32_t))
  {
    uint32_t word = 0U;
    memcpy(&word, bytes, sizeof(word));
    if (flash_program_word(address, word) != 0)
    {
      flash_lock();
      return -1;
    }
    address += sizeof(word);
    bytes += sizeof(word);
    remaining -= sizeof(word);
  }

  if (remaining != 0U)
  {
    uint32_t word = *(const uint32_t *)address;
    memcpy(&word, bytes, remaining);
    if (flash_program_word(address, word) != 0)
    {
      flash_lock();
      return -1;
    }
  }

  flash_lock();
  return 0;
}

int boot_flash_resident_verify_sha1(uint32_t base, size_t length, const uint8_t expected_sha1[20])
{
  if ((expected_sha1 == 0) || (length == 0U))
  {
    return -1;
  }

  if (!boot_mem_is_resident_range(base, (uint32_t)length))
  {
    return -1;
  }

  uint8_t digest[BOOT_SHA1_DIGEST_BYTES];
  boot_sha1_compute((const void *)base, length, digest);
  return (memcmp(digest, expected_sha1, BOOT_SHA1_DIGEST_BYTES) == 0) ? 0 : -1;
}
